/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef TEST_RAW_COMPACT_REGRESSIONS_H_
#define TEST_RAW_COMPACT_REGRESSIONS_H_

#include "aether-tele/raw.h"
#include "aether-tele/raw/decode.h"
#include "aether-tele/raw/metadata.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <unity.h>

namespace compact_regressions {
using namespace ae::tele::raw;
struct Identity {};
using S = Space<Identity>;
inline constexpr Tag<S> point{7, 1, 1, "point", "point", __FILE__, __LINE__};

struct Destination {
  std::vector<std::uint8_t> bytes;
  std::vector<std::vector<std::uint8_t>> attempts;
  unsigned failures{};
  unsigned fail_on_attempt{};
};
inline bool Save(void* context, std::span<std::uint8_t const> bytes) {
  auto& destination = *static_cast<Destination*>(context);
  destination.attempts.emplace_back(bytes.begin(), bytes.end());
  if (destination.attempts.size() == destination.fail_on_attempt) return false;
  if (destination.failures != 0) {
    --destination.failures;
    return false;
  }
  destination.bytes.insert(destination.bytes.end(), bytes.begin(), bytes.end());
  return true;
}
inline void AssertMetric(Metric const& expected, Metric const& actual) {
  TEST_ASSERT_EQUAL_UINT64(expected.count, actual.count);
  TEST_ASSERT_EQUAL_UINT64(expected.samples, actual.samples);
  TEST_ASSERT_EQUAL_UINT64(expected.sum, actual.sum);
  TEST_ASSERT_EQUAL_UINT64(expected.min, actual.min);
  TEST_ASSERT_EQUAL_UINT64(expected.max, actual.max);
}

inline void test_seed_is_not_a_new_delta() {
  S space;
  Destination destination;
  Stream<S, 28> stream(space, 4, Save, &destination);
  std::array seed{MetricTotal{7, Metric{10, 2, 30, 10, 20}}};
  TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
  AssertMetric(seed[0].metric, stream.MetricFor(7));
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(destination.attempts.empty());
  stream.Count(7);
  stream.AddDuration(7, 5);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  AssertMetric(Metric{11, 3, 35, 5, 20}, stream.MetricFor(7));
  auto decoded = Decode(destination.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
  AssertMetric(Metric{1, 1, 5, 5, 5}, *decoded.records[0].metric);
}

inline void test_seed_survives_failed_drain_and_later_updates() {
  S space;
  Destination destination;
  Stream<S, 28> stream(space, 4, Save, &destination);
  std::array seed{MetricTotal{7, Metric{10, 2, 30, 10, 20}}};
  TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
  stream.Count(7);
  stream.AddDuration(7, 5);
  destination.failures = 2;
  TEST_ASSERT_FALSE(stream.DrainMetrics());
  stream.Count(7);
  stream.AddDuration(7, 9);
  TEST_ASSERT_FALSE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_EQUAL_UINT(4, destination.attempts.size());
  TEST_ASSERT_EQUAL_MEMORY(destination.attempts[0].data(),
                           destination.attempts[1].data(),
                           destination.attempts[0].size());
  TEST_ASSERT_EQUAL_MEMORY(destination.attempts[0].data(),
                           destination.attempts[2].data(),
                           destination.attempts[0].size());
  auto decoded = Decode(destination.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, decoded.records.size());
  AssertMetric(Metric{2, 2, 14, 5, 9}, RestoreMetric(decoded.records, 7));
  AssertMetric(Metric{12, 4, 44, 5, 20}, stream.MetricFor(7));
}

inline void test_invalid_seeds_are_atomic_and_can_be_retried() {
  S space;
  Stream<S, 28, 2> stream(space, 4);
  std::array too_many{MetricTotal{7, Metric{1}}, MetricTotal{8, Metric{2}},
                      MetricTotal{9, Metric{3}}};
  TEST_ASSERT_FALSE(stream.RestoreMetrics(too_many));
  AssertMetric({}, stream.MetricFor(7));
  AssertMetric({}, stream.MetricFor(8));
  std::array duplicates{MetricTotal{7, Metric{1}}, MetricTotal{7, Metric{2}}};
  TEST_ASSERT_FALSE(stream.RestoreMetrics(duplicates));
  AssertMetric({}, stream.MetricFor(7));
  std::array bad_range{MetricTotal{7, Metric{1}},
                       MetricTotal{8, Metric{2, 1, 5, 9, 3}}};
  TEST_ASSERT_FALSE(stream.RestoreMetrics(bad_range));
  AssertMetric({}, stream.MetricFor(7));
  std::array valid{MetricTotal{7, Metric{10}}, MetricTotal{8, Metric{20}}};
  TEST_ASSERT_TRUE(stream.RestoreMetrics(valid));
  AssertMetric(valid[0].metric, stream.MetricFor(7));
  AssertMetric(valid[1].metric, stream.MetricFor(8));
  TEST_ASSERT_EQUAL_UINT64(0, stream.metric_overflow());
}

inline void test_seed_is_startup_only_and_never_replaces_live_data() {
  std::array seed{MetricTotal{7, Metric{10}}};
  for (unsigned operation = 0; operation != 7; ++operation) {
    S space;
    Destination destination;
    Stream<S, 28> stream(space, 4, Save, &destination);
    switch (operation) {
      case 0:
        stream.Count(7);
        break;
      case 1:
        stream.AddDuration(7, 5);
        break;
      case 2:
        TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog,
                                     Options{true, false, false, false, 0}));
        break;
      case 3:
        TEST_ASSERT_TRUE(stream.Drain());
        break;
      case 4:
        TEST_ASSERT_TRUE(stream.DrainMetrics());
        break;
      case 5:
        TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
        break;
      case 6:
        TEST_ASSERT_TRUE(stream.RestoreMetrics({}));
        break;
    }
    auto before = stream.MetricFor(7);
    TEST_ASSERT_FALSE(stream.RestoreMetrics(seed));
    AssertMetric(before, stream.MetricFor(7));
  }
}

inline void test_seed_and_new_deltas_saturate_independently() {
  S space;
  Destination destination;
  Stream<S, 28> stream(space, 4, Save, &destination);
  auto const maximum = std::numeric_limits<std::uint64_t>::max();
  std::array seed{MetricTotal{7, Metric{maximum, maximum, maximum - 1, 2, 10}}};
  TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
  stream.Count(7);
  stream.AddDuration(7, 5);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  AssertMetric(Metric{maximum, maximum, maximum, 2, 10}, stream.MetricFor(7));
  auto decoded = Decode(destination.bytes);
  AssertMetric(Metric{1, 1, 5, 5, 5}, RestoreMetric(decoded.records, 7));
}

inline void test_timing_only_scope_closes_restore_before_completion() {
  S space;
  Stream<S, 28> stream(space, 4);
  std::array seed{MetricTotal{7, Metric{10}}};
  {
    Scope scope(stream, point, Options{false, false, false, true, 0});
    TEST_ASSERT_FALSE(stream.RestoreMetrics(seed));
    AssertMetric({}, stream.MetricFor(7));
  }
  TEST_ASSERT_EQUAL_UINT64(1, stream.MetricFor(7).samples);
  Stream<S, 28> inactive(space, 4);
  {
    Scope scope(inactive, point, Options{false, false, false, false, 0});
    TEST_ASSERT_TRUE(inactive.RestoreMetrics(seed));
  }
  AssertMetric(seed[0].metric, inactive.MetricFor(7));
}

inline void test_disabled_collection_keeps_restored_history() {
  std::array seed{MetricTotal{7, Metric{10, 2, 30, 10, 20}}};
  S space;
  space.SetRuntime(Options{false, false, false, false, 0});
  Destination destination;
  Stream<S, 28> stream(space, 4, Save, &destination);
  TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
  Log(stream, point, space.Runtime());
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  AssertMetric(seed[0].metric, stream.MetricFor(7));
  TEST_ASSERT_TRUE(destination.bytes.empty());
  space.SetRuntime(Options{false, false, true, false, 0});
  Log(stream, point, space.Runtime());
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  AssertMetric(Metric{11, 2, 30, 10, 20}, stream.MetricFor(7));
  AssertMetric(Metric{1}, RestoreMetric(Decode(destination.bytes).records, 7));

  using Off = Space<Identity, Options{false, false, false, false, 0}>;
  Off disabled;
  Stream<Off, 28> disabled_stream(disabled, 4, Save, &destination);
  TEST_ASSERT_TRUE(disabled_stream.RestoreMetrics(seed));
  AssertMetric(seed[0].metric, disabled_stream.MetricFor(7));
  auto const saved = destination.bytes.size();
  TEST_ASSERT_TRUE(disabled_stream.DrainMetrics());
  TEST_ASSERT_EQUAL_UINT(saved, destination.bytes.size());
}

inline void test_repeated_restart_accumulates_only_persisted_deltas() {
  Destination destination;
  Metric expected;
  for (unsigned restart = 0; restart != 5; ++restart) {
    auto decoded = Decode(destination.bytes);
    TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
    auto restored = RestoreMetric(decoded.records, 7);
    AssertMetric(expected, restored);
    S space;
    Stream<S, 28> stream(space, 4, Save, &destination);
    std::array seed{MetricTotal{7, restored}};
    TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
    TEST_ASSERT_TRUE(stream.DrainMetrics());
    AssertMetric(expected, stream.MetricFor(7));
    stream.Count(7);
    stream.AddDuration(7, restart + 1);
    destination.failures = 1;
    TEST_ASSERT_FALSE(stream.DrainMetrics());
    TEST_ASSERT_TRUE(stream.DrainMetrics());
    ++expected.count;
    ++expected.samples;
    expected.sum += restart + 1;
    expected.min = 1;
    expected.max = restart + 1;
    AssertMetric(expected, stream.MetricFor(7));
    // A process exit loses this update; no stop marker or successful drain.
    stream.Count(7);
    stream.AddDuration(7, 999);
    destination.failures = 1;
    TEST_ASSERT_FALSE(stream.DrainMetrics());
  }
  AssertMetric(Metric{5, 5, 15, 1, 5},
               RestoreMetric(Decode(destination.bytes).records, 7));
}
inline void test_partial_multitag_metric_failure_preserves_disjoint_deltas() {
  S space;
  Destination destination;
  Stream<S, 64> stream(space, 4, Save, &destination);
  stream.Count(7);
  stream.AddDuration(7, 5);
  stream.Count(8);
  stream.AddDuration(8, 10);
  destination.fail_on_attempt = 2;
  TEST_ASSERT_FALSE(stream.DrainMetrics());
  AssertMetric({1, 1, 5, 5, 5},
               RestoreMetric(Decode(destination.bytes).records, 7));
  AssertMetric({}, RestoreMetric(Decode(destination.bytes).records, 8));
  stream.Count(7);
  stream.AddDuration(7, 7);
  stream.Count(8);
  stream.AddDuration(8, 12);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(destination.attempts[1] == destination.attempts[2]);
  auto decoded = Decode(destination.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(4, decoded.records.size());
  AssertMetric({2, 2, 12, 5, 7}, RestoreMetric(decoded.records, 7));
  AssertMetric({2, 2, 22, 10, 12}, RestoreMetric(decoded.records, 8));
  auto committed = destination.bytes.size();
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_EQUAL_UINT(committed, destination.bytes.size());
}

inline void test_failed_metric_retry_precedes_newer_logs() {
  S space;
  Destination destination;
  Stream<S, 64> stream(space, 4, Save, &destination);
  stream.Count(7);
  stream.AddDuration(7, 5);
  destination.failures = 1;
  TEST_ASSERT_FALSE(stream.DrainMetrics());
  stream.Count(7);
  stream.AddDuration(7, 7);
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog,
                               Options{true, false, false, false, kTime}, {}, 0,
                               3000));
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(destination.attempts[0] == destination.attempts[1]);
  auto decoded = Decode(destination.bytes);
  TEST_ASSERT_EQUAL_UINT(3, decoded.records.size());
  TEST_ASSERT_TRUE(decoded.records[0].kind == Kind::kMetric);
  TEST_ASSERT_TRUE(decoded.records[1].kind == Kind::kLog);
  TEST_ASSERT_TRUE(decoded.records[2].kind == Kind::kMetric);
  for (std::size_t i = 1; i < decoded.records.size(); ++i)
    TEST_ASSERT_TRUE(decoded.records[i - 1].sequence <
                     decoded.records[i].sequence);
  AssertMetric({2, 2, 12, 5, 7}, RestoreMetric(decoded.records, 7));
}

inline void test_reentrant_callbacks_do_not_overwrite_or_restore_buffers() {
  S space;
  using Writer = Stream<S, 64>;
  struct Context {
    Writer* stream{};
    std::vector<std::uint8_t> bytes;
    bool unchanged{true}, nested_emit{true}, nested_drain{true};
    bool nested_metrics{true}, nested_restore{true};
  } context;
  auto callback = +[](void* data, std::span<std::uint8_t const> bytes) {
    auto& c = *static_cast<Context*>(data);
    std::vector<std::uint8_t> original(bytes.begin(), bytes.end());
    c.nested_emit = c.stream->Emit(point, Kind::kLog,
                                   Options{true, false, false, false, 0});
    c.nested_drain = c.stream->Drain();
    c.nested_metrics = c.stream->DrainMetrics();
    c.nested_restore = c.stream->RestoreMetrics({});
    c.stream->Count(7);
    c.stream->AddDuration(7, 99);
    c.unchanged =
        original == std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    c.bytes.insert(c.bytes.end(), bytes.begin(), bytes.end());
    return c.unchanged;
  };
  Writer stream(space, 4, callback, &context);
  context.stream = &stream;
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, Options{true, false, false, false, 0}));
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_TRUE(context.unchanged);
  TEST_ASSERT_FALSE(context.nested_emit);
  TEST_ASSERT_FALSE(context.nested_drain);
  TEST_ASSERT_FALSE(context.nested_metrics);
  TEST_ASSERT_FALSE(context.nested_restore);
  TEST_ASSERT_EQUAL_UINT64(1, stream.dropped());
  TEST_ASSERT_EQUAL_UINT64(2, stream.metric_overflow());
  AssertMetric({}, stream.MetricFor(7));
  TEST_ASSERT_EQUAL_UINT(1, Decode(context.bytes).records.size());
}

inline void test_throwing_callbacks_preserve_committed_record() {
  S space;
  Destination destination;
  bool fail = true;
  struct Context {
    Destination* destination;
    bool* fail;
  } context{&destination, &fail};
  auto drain = +[](void* data, std::span<std::uint8_t const> bytes) {
    auto& c = *static_cast<Context*>(data);
    if (*c.fail) throw std::runtime_error("drain failed");
    return Save(c.destination, bytes);
  };
  auto console = +[](void*, std::span<std::uint8_t const>) {
    throw std::runtime_error("console failed");
  };
  Stream<S, 64> stream(space, 4, drain, &context, console);
  TEST_ASSERT_FALSE(
      stream.Emit(point, Kind::kLog, Options{true, true, false, false, 0}));
  auto pending = std::vector<std::uint8_t>(stream.pending().begin(),
                                           stream.pending().end());
  TEST_ASSERT_EQUAL_UINT64(1, stream.console_failures());
  TEST_ASSERT_FALSE(stream.Drain());
  TEST_ASSERT_TRUE(
      std::equal(pending.begin(), pending.end(), stream.pending().begin()));
  fail = false;
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL_UINT(1, Decode(destination.bytes).records.size());
  TEST_ASSERT_TRUE(destination.bytes == pending);
}

struct Clock {
  static inline std::uint64_t timestamp{1000}, steady{};
  static std::uint64_t TimestampUs() { return timestamp; }
  static std::uint64_t SteadyUs() { return steady; }
};

template <unsigned Mask>
inline void CompileCapability() {
  constexpr Options caps{bool(Mask & 1), bool(Mask & 2), bool(Mask & 4),
                         bool(Mask & 8), kAllFields};
  using C = Space<Identity, caps, Clock>;
  constexpr Tag<C> tag{1, 1, 1, "mask", "mask", __FILE__, __LINE__};
  C space;
  Destination disk, console;
  auto sink = +[](void* context, std::span<std::uint8_t const> bytes) {
    auto& d = *static_cast<Destination*>(context);
    d.bytes.insert(d.bytes.end(), bytes.begin(), bytes.end());
  };
  Stream<C, 128> stream(space, 4, Save, &disk, sink, &console);
  space.SetRuntime(Options{});
  TEST_ASSERT_TRUE(space.Runtime() == caps);
  Clock::steady = 10;
  {
    Scope scope(stream, tag, space.Runtime());
    Clock::steady = 30;
  }
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL(caps.logs, !Decode(disk.bytes).records.empty());
  TEST_ASSERT_EQUAL(caps.console, !Decode(console.bytes).records.empty());
  TEST_ASSERT_EQUAL_UINT64(caps.count ? 1 : 0, stream.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT64(caps.timing ? 1 : 0, stream.MetricFor(1).samples);
  TEST_ASSERT_EQUAL_UINT64(caps.timing ? 20 : 0, stream.MetricFor(1).sum);
}
template <unsigned... I>
inline void AllCapabilities(std::integer_sequence<unsigned, I...>) {
  (CompileCapability<I>(), ...);
}
inline void test_all_compile_capability_combinations() {
  AllCapabilities(std::make_integer_sequence<unsigned, 16>{});
}
inline void test_all_runtime_capability_combinations() {
  using C = Space<Identity, Options{}, Clock>;
  constexpr Tag<C> tag{1, 1, 1, "runtime", "runtime", __FILE__, __LINE__};
  for (unsigned mask = 0; mask != 16; ++mask) {
    C space;
    Destination disk, console;
    auto sink = +[](void* context, std::span<std::uint8_t const> bytes) {
      auto& d = *static_cast<Destination*>(context);
      d.bytes.insert(d.bytes.end(), bytes.begin(), bytes.end());
    };
    Stream<C, 128> stream(space, 4, Save, &disk, sink, &console);
    Options options{bool(mask & 1), bool(mask & 2), bool(mask & 4),
                    bool(mask & 8), kAllFields};
    space.SetRuntime(options);
    Clock::steady = 100;
    {
      Scope scope(stream, tag, space.Runtime());
      Clock::steady = 125;
    }
    TEST_ASSERT_TRUE(stream.Drain());
    TEST_ASSERT_EQUAL(options.logs, !Decode(disk.bytes).records.empty());
    TEST_ASSERT_EQUAL(options.console, !Decode(console.bytes).records.empty());
    TEST_ASSERT_EQUAL_UINT64(options.count ? 1 : 0, stream.MetricFor(1).count);
    TEST_ASSERT_EQUAL_UINT64(options.timing ? 25 : 0, stream.MetricFor(1).sum);
  }
}

template <std::uint32_t Mask>
inline void CompileFields() {
  constexpr Options caps{true, true, false, false, Mask};
  using C = Space<Identity, caps, Clock>;
  constexpr Tag<C> tag{1, 1, 1, "fields", "fields", __FILE__, __LINE__};
  C space;
  Stream<C, 128> stream(space, 4);
  space.SetRuntime(Options{});
  int evaluated = 0;
  std::array<std::uint8_t, 2> bytes{1, 2};
  AE_RAW_PAYLOAD(stream, tag,
                 (++evaluated, std::span<std::uint8_t const>{bytes}));
  auto decoded = Decode(stream.pending());
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT32(Mask, decoded.records[0].fields);
  TEST_ASSERT_EQUAL(bool(Mask & kTime), decoded.records[0].time.has_value());
  TEST_ASSERT_EQUAL_UINT(Mask & kPayload ? 2 : 0,
                         decoded.records[0].payload.size());
  TEST_ASSERT_EQUAL_INT(Mask & kPayload ? 1 : 0, evaluated);
  if (Mask & kDuration)
    TEST_ASSERT_EQUAL_UINT8(255, decoded.records[0].duration_code);
}
inline void test_all_compiled_field_combinations_and_lazy_payload() {
  CompileFields<0>();
  CompileFields<kTime>();
  CompileFields<kDuration>();
  CompileFields<kTime | kDuration>();
  CompileFields<kPayload>();
  CompileFields<kTime | kPayload>();
  CompileFields<kDuration | kPayload>();
  CompileFields<kAllFields>();
}

inline void test_instant_log_does_not_flush_active_scope() {
  using C = Space<Identity, Options{}, Clock>;
  constexpr Tag<C> tag{1, 1, 1, "scope", "scope", __FILE__, __LINE__};
  C space;
  space.SetRuntime({true, false, false, false, kTime | kDuration});
  Destination destination;
  Stream<C, 256> stream(space, 4, Save, &destination);
  Clock::steady = 0;
  {
    Scope outer(stream, tag, space.Runtime());
    Clock::steady = 10;
    Log(stream, tag, space.Runtime());
    {
      Scope inner(stream, tag, space.Runtime());
      Clock::steady = 30;
    }
    Clock::steady = 50;
  }
  TEST_ASSERT_TRUE(destination.attempts.empty());
  auto decoded = Decode(stream.pending());
  TEST_ASSERT_EQUAL_UINT(3, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(50),
                          decoded.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT8(255, decoded.records[1].duration_code);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(20),
                          decoded.records[2].duration_code);
}

inline void test_failed_alias_auto_drain_retains_exact_pending_bytes() {
  S space;
  Destination destination;
  Stream<S, 128> stream(space, 4, Save, &destination);
  Options options{true, false, false, false, kPayload};
  std::array<std::uint8_t, 64> payload{};
  payload.fill(7);
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, options, Data{payload}));
  auto original = std::vector<std::uint8_t>(stream.pending().begin(),
                                            stream.pending().end());
  destination.failures = 1;
  TEST_ASSERT_FALSE(
      stream.Emit(point, Kind::kLog, options, Data{stream.pending()}));
  TEST_ASSERT_EQUAL_UINT64(1, stream.dropped());
  TEST_ASSERT_EQUAL_UINT(original.size(), stream.pending().size());
  TEST_ASSERT_EQUAL_MEMORY(original.data(), stream.pending().data(),
                           original.size());
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_TRUE(destination.bytes == original);
}

struct TemporaryPayload {
  static inline std::array<std::uint8_t, 4> bytes{};
  TemporaryPayload() { bytes = {11, 22, 33, 44}; }
  ~TemporaryPayload() { bytes.fill(0xee); }
  operator std::span<std::uint8_t const>() const { return bytes; }
};
inline void test_macro_temporary_lifetime_and_stream_name_hygiene() {
  S space;
  space.SetRuntime({true, false, false, false, kPayload});
  Stream<S, 128> ae_raw_stream(space, 4);
  AE_RAW_PAYLOAD(ae_raw_stream, point, TemporaryPayload{});
  {
    AE_RAW_SCOPE(ae_raw_stream, point);
  }
  TEST_ASSERT_EQUAL_UINT8(0xee, TemporaryPayload::bytes[0]);
  auto decoded = Decode(ae_raw_stream.pending());
  TEST_ASSERT_EQUAL_UINT(2, decoded.records.size());
  constexpr std::array<std::uint8_t, 4> expected{11, 22, 33, 44};
  TEST_ASSERT_EQUAL_UINT(expected.size(), decoded.records[0].payload.size());
  TEST_ASSERT_EQUAL_MEMORY(expected.data(), decoded.records[0].payload.data(),
                           expected.size());
}

inline void test_metadata_schema_reindex_and_static_labels_only_in_map() {
  struct MapIdentity {};
  using M = Space<MapIdentity>;
  M space;
  std::array tags{Tag<M>{7, 3, 2, "tag", "original", "decl.cpp", 1},
                  Tag<M>{8, 3, 2, "other", "other", "decl.cpp", 2}};
  Stream<M, 128> original(space, 4), changed(space, 4);
  auto modified = tags[0];
  modified.level = 123;
  modified.module = 456;
  modified.text = "changed static text";
  modified.file = "other.cpp";
  modified.line = 1234;
  Options options{true, false, false, false, kAllFields};
  TEST_ASSERT_TRUE(original.Emit(tags[0], Kind::kLog, options, {}, 456, 123));
  TEST_ASSERT_TRUE(changed.Emit(modified, Kind::kLog, options, {}, 456, 123));
  TEST_ASSERT_EQUAL_UINT(original.pending().size(), changed.pending().size());
  TEST_ASSERT_EQUAL_MEMORY(original.pending().data(), changed.pending().data(),
                           original.pending().size());
  TEST_ASSERT_TRUE(RecordSite(tags[0], "caller.cpp", 42));
  std::ostringstream map;
  TEST_ASSERT_TRUE(
      ExportMap(map, "test", "build-A", std::span<Tag<M> const>{tags}));
  for (auto required :
       {"\"layout\":\"compact_blocks\"", "\"minimum_block_bytes\":6",
        "\"time_values_us\":[", "\"duration_values_us\":[",
        "\"numeric_codecs\":", "caller.cpp"})
    TEST_ASSERT_TRUE(map.str().find(required) != std::string::npos);
  TEST_ASSERT_TRUE(map.str().find("\"1\":\"duration\"") == std::string::npos);
  tags[0].index = 2000;
  tags[1].index = 1;
  std::ostringstream reindexed;
  TEST_ASSERT_TRUE(
      ExportMap(reindexed, "test", "build-B", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(reindexed.str().find("\"index\":2000") != std::string::npos);
  tags[1].index = 2000;
  std::ostringstream duplicate;
  TEST_ASSERT_FALSE(
      ExportMap(duplicate, "test", "build-C", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(duplicate.str().empty());
  tags[1].index = 65536;
  std::ostringstream range;
  TEST_ASSERT_FALSE(
      ExportMap(range, "test", "build-C", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(range.str().empty());
  tags[1].index = 1;
  tags[1].text = std::string_view{"\xff", 1};
  std::ostringstream encoding;
  TEST_ASSERT_FALSE(
      ExportMap(encoding, "test", "build-C", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(encoding.str().empty());
}

inline void
test_unique_metadata_callsites_are_thread_safe_and_conflicts_sticky() {
  struct MapIdentity {};
  using M = Space<MapIdentity>;
  constexpr Tag<M> tag{52, 2, 1, "unique", "threaded", "decl.cpp", 11};
  std::atomic<bool> failed{false};
  std::array<std::thread, 4> writers;
  for (auto& writer : writers)
    writer = std::thread([&] {
      for (unsigned i = 0; i != 100; ++i)
        if (!RecordSite(tag, "caller.cpp", 44)) failed = true;
    });
  for (auto& writer : writers) writer.join();
  TEST_ASSERT_FALSE(failed.load());
  std::array tags{tag};
  std::ostringstream valid;
  TEST_ASSERT_TRUE(
      ExportMap(valid, "test", "build", std::span<Tag<M> const>{tags}));
  tags[0].text = "conflicting description";
  std::ostringstream labels;
  TEST_ASSERT_FALSE(
      ExportMap(labels, "test", "build", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(labels.str().empty());
  tags[0] = tag;
  TEST_ASSERT_FALSE(RecordSite(tag, "other.cpp", 45));
  TEST_ASSERT_FALSE(RecordSite(tag, "caller.cpp", 44));
  std::ostringstream ambiguous;
  TEST_ASSERT_FALSE(
      ExportMap(ambiguous, "test", "build", std::span<Tag<M> const>{tags}));
  TEST_ASSERT_TRUE(ambiguous.str().empty());
}

inline void
test_corrupt_controls_ranges_and_visible_lengths_are_not_repairable() {
  std::array<std::uint8_t, 128> bytes{};
  std::size_t cursor = 5;
  TEST_ASSERT_TRUE(wire::Write<wire::StreamCodec>(4, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::SequenceCodec>(0, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::TagCodec>(7, bytes, cursor));
  auto block = wire::FinishBlock(bytes, cursor);
  for (std::uint8_t invalid : {2, 3, 16, 64, 5, 9, 33}) {
    bytes[4] = invalid;
    for (std::size_t cut = 2; cut <= block.size(); ++cut)
      TEST_ASSERT_TRUE(Decode(block.first(cut)).status ==
                       DecodeStatus::kMalformed);
  }
  // A visible payload count larger than its declared block cannot be repaired
  // by dropping a missing physical tail.
  cursor = 5;
  bytes[4] = wire::Control(wire::kLogBlock, kPayload);
  TEST_ASSERT_TRUE(wire::Write<wire::StreamCodec>(4, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::SequenceCodec>(0, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::TagCodec>(7, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::BlobCodec>(100, bytes, cursor));
  block = wire::FinishBlock(bytes, cursor + 1);
  TEST_ASSERT_TRUE(Decode(block.first(block.size() - 1)).status ==
                   DecodeStatus::kMalformed);
  // Bypass logical writer limits to check hostile on-disk tag and stream IDs.
  cursor = 5;
  bytes[4] = 0;
  cursor += wire::StreamCodec{65536}.Serialize(bytes.data() + cursor);
  TEST_ASSERT_TRUE(wire::Write<wire::SequenceCodec>(0, bytes, cursor));
  bytes[cursor++] = 0;
  block = wire::FinishBlock(bytes, cursor);
  TEST_ASSERT_TRUE(Decode(block).status == DecodeStatus::kMalformed);
  cursor = 5;
  TEST_ASSERT_TRUE(wire::Write<wire::StreamCodec>(4, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::SequenceCodec>(0, bytes, cursor));
  cursor += wire::TagCodec{65536}.Serialize(bytes.data() + cursor);
  block = wire::FinishBlock(bytes, cursor);
  TEST_ASSERT_TRUE(Decode(block).status == DecodeStatus::kMalformed);
}

inline void test_runtime_changes_concurrently_with_owner_writers() {
  // SystemClock has no shared mutable fake state. Every Stream and destination
  // has one writer; only the atomic runtime configuration is shared.
  struct RuntimeIdentity {};
  using R = Space<RuntimeIdentity>;
  constexpr Tag<R> scope_tag{7, 1, 1, "scope", "scope", __FILE__, __LINE__};
  constexpr Tag<R> log_tag{8, 1, 1, "log", "log", __FILE__, __LINE__};
  constexpr Options a{true, false, true, true, kAllFields};
  constexpr Options b{false, true, false, false, kTime};
  R space;
  space.SetRuntime(a);
  std::array<Destination, 4> disk, console;
  std::array<Metric, 4> scope_totals, log_totals;
  std::array<std::thread, 4> writers;
  std::atomic<bool> go{false}, failed{false};
  auto console_sink = +[](void* data, std::span<std::uint8_t const> bytes) {
    auto& destination = *static_cast<Destination*>(data);
    destination.bytes.insert(destination.bytes.end(), bytes.begin(),
                             bytes.end());
  };
  std::thread config([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (unsigned i = 0; i != 20000; ++i) {
      space.SetRuntime(i & 1 ? a : b);
      if ((i & 63) == 0) std::this_thread::yield();
    }
  });
  for (std::size_t id = 0; id < writers.size(); ++id) {
    writers[id] = std::thread([&, id] {
      Stream<R, 128> stream(space, static_cast<std::uint32_t>(id), Save,
                            &disk[id], console_sink, &console[id]);
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (unsigned i = 0; i != 1000; ++i) {
        auto snapshot = space.Runtime();
        if (snapshot != a && snapshot != b) failed = true;
        Scope scope(stream, scope_tag, snapshot);
        std::array<std::uint8_t, 4> payload{};
        Store32(payload.data(), i);
        snapshot = space.Runtime();
        if (snapshot != a && snapshot != b) failed = true;
        Log(stream, log_tag, snapshot, Data{payload});
      }
      if (!stream.DrainMetrics() || stream.dropped() != 0 ||
          stream.metric_overflow() != 0 || stream.console_failures() != 0)
        failed = true;
      scope_totals[id] = stream.MetricFor(scope_tag.index);
      log_totals[id] = stream.MetricFor(log_tag.index);
    });
  }
  go.store(true, std::memory_order_release);
  config.join();
  for (auto& writer : writers) writer.join();
  TEST_ASSERT_FALSE(failed.load());
  for (std::size_t id = 0; id < writers.size(); ++id) {
    auto decoded = Decode(disk[id].bytes);
    TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
    TEST_ASSERT_EQUAL_UINT(disk[id].bytes.size(), decoded.valid_bytes);
    AssertMetric(scope_totals[id],
                 RestoreMetric(decoded.records, scope_tag.index));
    AssertMetric(log_totals[id], RestoreMetric(decoded.records, log_tag.index));
    TEST_ASSERT_EQUAL_UINT64(scope_totals[id].count, scope_totals[id].samples);
    std::uint64_t scope_rows = 0, log_rows = 0;
    for (auto const& record : decoded.records) {
      TEST_ASSERT_EQUAL_UINT(id, record.stream);
      if (record.kind == Kind::kMetric) continue;
      TEST_ASSERT_EQUAL_UINT32(kAllFields, record.fields);
      if (record.index == scope_tag.index)
        ++scope_rows;
      else
        ++log_rows;
    }
    TEST_ASSERT_EQUAL_UINT64(scope_totals[id].count, scope_rows);
    TEST_ASSERT_EQUAL_UINT64(log_totals[id].count, log_rows);
    auto visible = Decode(console[id].bytes);
    TEST_ASSERT_TRUE(visible.status == DecodeStatus::kOk);
    for (auto const& record : visible.records) {
      TEST_ASSERT_EQUAL_UINT32(kTime, record.fields);
      TEST_ASSERT_TRUE(record.time.has_value());
      TEST_ASSERT_FALSE(record.duration.has_value());
      TEST_ASSERT_TRUE(record.payload.empty());
    }
  }
}

inline void
test_record_formatter_respects_disabled_fields_and_duration_states() {
  for (std::uint32_t fields = 0; fields <= kAllFields; ++fields) {
    if (fields & ~kAllFields) continue;
    S space;
    Stream<S, 128> stream(space, 4);
    std::array<std::uint8_t, 2> payload{1, 2};
    TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog,
                                 Options{true, false, false, false, fields},
                                 Data{payload}, 123, 456));
    auto decoded = Decode(stream.pending());
    TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
    std::ostringstream text;
    PrintRecord(text, decoded.records[0]);
    TEST_ASSERT_EQUAL(bool(fields & kTime),
                      text.str().find("time_us=") != std::string::npos);
    TEST_ASSERT_EQUAL(bool(fields & kDuration),
                      text.str().find("duration_us=") != std::string::npos);
    TEST_ASSERT_EQUAL(bool(fields & kPayload),
                      text.str().find("payload_bytes=") != std::string::npos);
  }
  DecodedRecord special;
  special.fields = kDuration;
  for (auto state : {DurationState::kBelowMin, DurationState::kAboveMax,
                     DurationState::kUnknown}) {
    special.duration_state = state;
    std::ostringstream text;
    PrintRecord(text, special);
    TEST_ASSERT_TRUE(text.str().find("duration=") != std::string::npos);
    TEST_ASSERT_TRUE(text.str().find("duration_us=") == std::string::npos);
  }
}

inline void RunTests() {
  RUN_TEST(test_seed_is_not_a_new_delta);
  RUN_TEST(test_seed_survives_failed_drain_and_later_updates);
  RUN_TEST(test_invalid_seeds_are_atomic_and_can_be_retried);
  RUN_TEST(test_seed_is_startup_only_and_never_replaces_live_data);
  RUN_TEST(test_seed_and_new_deltas_saturate_independently);
  RUN_TEST(test_timing_only_scope_closes_restore_before_completion);
  RUN_TEST(test_disabled_collection_keeps_restored_history);
  RUN_TEST(test_repeated_restart_accumulates_only_persisted_deltas);
  RUN_TEST(test_partial_multitag_metric_failure_preserves_disjoint_deltas);
  RUN_TEST(test_failed_metric_retry_precedes_newer_logs);
  RUN_TEST(test_reentrant_callbacks_do_not_overwrite_or_restore_buffers);
  RUN_TEST(test_throwing_callbacks_preserve_committed_record);
  RUN_TEST(test_all_compile_capability_combinations);
  RUN_TEST(test_all_runtime_capability_combinations);
  RUN_TEST(test_all_compiled_field_combinations_and_lazy_payload);
  RUN_TEST(test_instant_log_does_not_flush_active_scope);
  RUN_TEST(test_failed_alias_auto_drain_retains_exact_pending_bytes);
  RUN_TEST(test_macro_temporary_lifetime_and_stream_name_hygiene);
  RUN_TEST(test_metadata_schema_reindex_and_static_labels_only_in_map);
  RUN_TEST(test_unique_metadata_callsites_are_thread_safe_and_conflicts_sticky);
  RUN_TEST(test_corrupt_controls_ranges_and_visible_lengths_are_not_repairable);
  RUN_TEST(test_runtime_changes_concurrently_with_owner_writers);
  RUN_TEST(test_record_formatter_respects_disabled_fields_and_duration_states);
}
}  // namespace compact_regressions
#endif  // TEST_RAW_COMPACT_REGRESSIONS_H_
