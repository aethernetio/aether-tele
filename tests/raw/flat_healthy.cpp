/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include "aether-tele/raw.h"
#include "aether-tele/raw/decode.h"
#ifndef AE_TELE_TEST_NATIVE_FILE
#  define AE_TELE_TEST_NATIVE_FILE 1
#endif
#if AE_TELE_TEST_NATIVE_FILE
#  include "aether-tele/raw/file.h"
#endif
#include "aether-tele/raw/metadata.h"
#include "aether-tele/raw/wire.h"

#include <unity.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> track_allocations{false};
std::atomic<unsigned> allocations{0};
}  // namespace
void* operator new(std::size_t n) {
  if (track_allocations.load()) ++allocations;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace ae::tele::raw;
namespace fs = std::filesystem;
struct Clock {
  static inline std::uint64_t time{1000}, steady{};
  static inline unsigned reads{};
  static std::uint64_t TimestampUs() {
    ++reads;
    return time;
  }
  static std::uint64_t SteadyUs() {
    ++reads;
    return steady;
  }
};
struct Identity {};
using S = Space<Identity, Options{}, Clock>;
#ifndef RAW_TEST_TAG_INDEX
#  define RAW_TEST_TAG_INDEX 7
#endif
constexpr Tag<S> point{RAW_TEST_TAG_INDEX, 3,       2, "point", "work",
                       __FILE__,           __LINE__};
constexpr Tag<S> other{8, 3, 2, "other", "nested work", __FILE__, __LINE__};
constexpr Tag<S> start{100,      3,       2, "start", "application start",
                       __FILE__, __LINE__};

struct Destination {
  std::vector<std::uint8_t> bytes, console;
  std::vector<std::vector<std::uint8_t>> attempts;
  bool accept{true}, throws{};
  std::array<std::uint8_t, 64>* mutate{};
};
bool Save(void* context, std::span<std::uint8_t const> bytes) {
  auto& d = *static_cast<Destination*>(context);
  d.attempts.emplace_back(bytes.begin(), bytes.end());
  if (d.mutate) d.mutate->fill(0xee);
  if (d.throws) throw std::runtime_error("storage failed");
  if (!d.accept) return false;
  d.bytes.insert(d.bytes.end(), bytes.begin(), bytes.end());
  return true;
}
void Console(void* context, std::span<std::uint8_t const> bytes) {
  auto& d = *static_cast<Destination*>(context);
  d.console.insert(d.console.end(), bytes.begin(), bytes.end());
}
bool Discard(void*, std::span<std::uint8_t const>) { return true; }
void AssertMetric(Metric const& expected, Metric const& actual) {
  TEST_ASSERT_EQUAL_UINT64(expected.count, actual.count);
  TEST_ASSERT_EQUAL_UINT64(expected.samples, actual.samples);
  TEST_ASSERT_EQUAL_UINT64(expected.sum, actual.sum);
  TEST_ASSERT_EQUAL_UINT64(expected.min, actual.min);
  TEST_ASSERT_EQUAL_UINT64(expected.max, actual.max);
}
bool SameMetric(Metric const& a, Metric const& b) {
  return a.count == b.count && a.samples == b.samples && a.sum == b.sum &&
         a.min == b.min && a.max == b.max;
}
std::vector<std::uint8_t> Copy(std::span<std::uint8_t const> bytes) {
  return {bytes.begin(), bytes.end()};
}
void AssertDecoded(DecodeResult const& result, std::size_t count) {
  TEST_ASSERT_TRUE(result.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(count, result.records.size());
}

void test_nested_duration_without_time_patches_each_reserved_byte() {
  S space;
  space.SetRuntime({true, false, false, false, kDuration});
  Stream<S, 128> stream(space, 1);
  Clock::steady = 0;
  {
    Scope outer(stream, point, space.Runtime());
    Clock::steady = 10;
    {
      Scope inner(stream, other, space.Runtime());
      auto active = Decode(stream.pending());
      AssertDecoded(active, 2);
      TEST_ASSERT_EQUAL_UINT8(255, active.records[0].duration_code);
      TEST_ASSERT_EQUAL_UINT8(255, active.records[1].duration_code);
      Clock::steady = 30;
    }
    auto partial = Decode(stream.pending());
    AssertDecoded(partial, 2);
    TEST_ASSERT_EQUAL_UINT8(255, partial.records[0].duration_code);
    TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(20),
                            partial.records[1].duration_code);
    Clock::steady = 50;
  }
  auto completed = Decode(stream.pending());
  AssertDecoded(completed, 2);
  TEST_ASSERT_EQUAL_UINT32(point.index, completed.records[0].index);
  TEST_ASSERT_EQUAL_UINT32(other.index, completed.records[1].index);
  TEST_ASSERT_EQUAL_UINT32(kDuration, completed.records[0].fields);
  TEST_ASSERT_EQUAL_UINT32(kDuration, completed.records[1].fields);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(50),
                          completed.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(20),
                          completed.records[1].duration_code);
}
void test_duration_ticket_cannot_patch_another_stream_instance() {
  S space;
  space.SetRuntime({true, false, false, false, kDuration});
  Stream<S, 128> first(space, 1), second(space, 1);
  Stream<S, 128>::DurationTicket first_ticket, second_ticket;
  TEST_ASSERT_TRUE(
      first.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 0, &first_ticket));
  TEST_ASSERT_TRUE(second.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 0,
                               &second_ticket));
  TEST_ASSERT_TRUE(first_ticket.valid);
  TEST_ASSERT_TRUE(second_ticket.valid);
  TEST_ASSERT_FALSE(first.FinishDuration(second_ticket, 30));
  TEST_ASSERT_FALSE(second.FinishDuration(first_ticket, 40));
  auto first_active = Decode(first.pending()),
       second_active = Decode(second.pending());
  AssertDecoded(first_active, 1);
  AssertDecoded(second_active, 1);
  TEST_ASSERT_EQUAL_UINT8(255, first_active.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT8(255, second_active.records[0].duration_code);
  TEST_ASSERT_TRUE(first.FinishDuration(first_ticket, 30));
  TEST_ASSERT_TRUE(second.FinishDuration(second_ticket, 40));
  TEST_ASSERT_FALSE(first.FinishDuration(first_ticket, 50));
  auto first_done = Decode(first.pending()),
       second_done = Decode(second.pending());
  AssertDecoded(first_done, 1);
  AssertDecoded(second_done, 1);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(30),
                          first_done.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(40),
                          second_done.records[0].duration_code);
}
void test_reconstruction_merges_streams_preserving_writer_order() {
  S space;
  space.SetRuntime({true, false, false, false, kTime});
  Destination d;
  Stream<S, 128> first(space, 1, Save, &d), second(space, 2, Save, &d);
  TEST_ASSERT_TRUE(first.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 1000));
  TEST_ASSERT_TRUE(first.Emit(other, Kind::kLog, space.Runtime(), {}, 0, 3000));
  TEST_ASSERT_TRUE(
      second.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 2000));
  TEST_ASSERT_TRUE(
      second.Emit(other, Kind::kLog, space.Runtime(), {}, 0, 4000));
  TEST_ASSERT_TRUE(second.Drain());
  TEST_ASSERT_TRUE(first.Drain());
  auto merged = Reconstruct(Decode(d.bytes).records);
  TEST_ASSERT_TRUE(merged.all_timestamps_present);
  TEST_ASSERT_FALSE(merged.clock_regressed);
  TEST_ASSERT_EQUAL_UINT(4, merged.records.size());
  for (unsigned i = 0; i != merged.records.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(1 + i % 2, merged.records[i].stream);
    TEST_ASSERT_EQUAL_UINT64(i / 2, merged.records[i].sequence);
  }
}
void test_duration_states_and_exact_endpoints() {
  for (auto const& item :
       std::array{std::pair{std::uint64_t{0}, std::uint8_t{0}},
                  std::pair{std::uint64_t{1}, std::uint8_t{1}},
                  std::pair{std::uint64_t{60'000'000}, std::uint8_t{253}},
                  std::pair{std::uint64_t{60'000'001}, std::uint8_t{254}}}) {
    S space;
    space.SetRuntime({true, false, true, true, kDuration});
    Stream<S, 128> stream(space, 1);
    Clock::steady = 10;
    {
      Scope scope(stream, point, space.Runtime());
      auto initial = Decode(stream.pending());
      TEST_ASSERT_TRUE(initial.status == DecodeStatus::kOk);
      TEST_ASSERT_EQUAL_UINT(1, initial.records.size());
      TEST_ASSERT_EQUAL_UINT8(255, initial.records[0].duration_code);
      TEST_ASSERT_FALSE(initial.records[0].duration.has_value());
      Clock::steady = 10 + item.first;
    }
    auto decoded = Decode(stream.pending());
    TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
    TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
    TEST_ASSERT_EQUAL_UINT8(item.second, decoded.records[0].duration_code);
    if (item.second == 1 || item.second == 253) {
      TEST_ASSERT_TRUE(decoded.records[0].duration.has_value());
      TEST_ASSERT_EQUAL_UINT64(item.first, *decoded.records[0].duration);
    } else
      TEST_ASSERT_FALSE(decoded.records[0].duration.has_value());
    AssertMetric({1, 1, item.first, item.first, item.first},
                 stream.MetricFor(point.index));
  }
  TEST_ASSERT_TRUE(wire::DecodeDuration(0).state == DurationState::kBelowMin);
  TEST_ASSERT_TRUE(wire::DecodeDuration(254).state == DurationState::kAboveMax);
  TEST_ASSERT_TRUE(wire::DecodeDuration(255).state == DurationState::kUnknown);
}
void test_nested_flat_scopes_patch_one_entry_each() {
  S space;
  space.SetRuntime({true, false, true, true, kTime | kDuration});
  Stream<S, 256> stream(space, 1);
  Clock::steady = 0;
  {
    Scope outer(stream, point, space.Runtime());
    Clock::steady = 10;
    {
      Scope inner(stream, other, space.Runtime());
      Clock::steady = 30;
    }
    Clock::steady = 50;
  }
  auto d = Decode(stream.pending());
  TEST_ASSERT_TRUE(d.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, d.records.size());
  TEST_ASSERT_TRUE(d.records[0].kind == Kind::kLog);
  TEST_ASSERT_TRUE(d.records[1].kind == Kind::kLog);
  TEST_ASSERT_TRUE(d.records[0].duration_state == DurationState::kValue);
  TEST_ASSERT_TRUE(d.records[1].duration_state == DurationState::kValue);
  AssertMetric({1, 1, 50, 50, 50}, stream.MetricFor(point.index));
  AssertMetric({1, 1, 20, 20, 20}, stream.MetricFor(8));
}
void test_drain_invalidates_open_scope_without_completion_record() {
  S space;
  space.SetRuntime({true, false, true, true, kDuration});
  Destination d;
  Stream<S, 128> stream(space, 1, Save, &d);
  Clock::steady = 0;
  {
    Scope scope(stream, point, space.Runtime());
    TEST_ASSERT_TRUE(stream.Drain());
    Clock::steady = 100;
  }
  TEST_ASSERT_TRUE(stream.pending().empty());
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL_UINT(1, d.attempts.size());
  auto decoded = Decode(d.bytes);
  TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT8(255, decoded.records[0].duration_code);
  AssertMetric({1, 1, 100, 100, 100}, stream.MetricFor(point.index));
}
void test_failed_drain_freezes_slot_and_retries_identical_bytes() {
  S space;
  space.SetRuntime({true, false, true, true, kDuration});
  Destination d;
  Stream<S, 128> stream(space, 1, Save, &d);
  Clock::steady = 0;
  {
    Scope scope(stream, point, space.Runtime());
    d.accept = false;
    TEST_ASSERT_FALSE(stream.Drain());
    Clock::steady = 100;
  }
  TEST_ASSERT_EQUAL_UINT(1, d.attempts.size());
  TEST_ASSERT_TRUE(Copy(stream.pending()) == d.attempts[0]);
  d.accept = true;
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL_UINT(2, d.attempts.size());
  TEST_ASSERT_TRUE(d.attempts[0] == d.attempts[1]);
  auto decoded = Decode(d.bytes);
  AssertDecoded(decoded, 1);
  TEST_ASSERT_EQUAL_UINT8(255, decoded.records[0].duration_code);
  AssertMetric({1, 1, 100, 100, 100}, stream.MetricFor(point.index));
}
void test_generation_prevents_old_handle_patching_reused_offset() {
  S space;
  space.SetRuntime({true, false, false, false, kDuration});
  Destination d;
  Stream<S, 128> stream(space, 1, Save, &d);
  using Scoped = Scope<Stream<S, 128>>;
  std::optional<Scoped> old;
  Clock::steady = 0;
  old.emplace(stream, point, space.Runtime());
  TEST_ASSERT_TRUE(stream.Drain());
  Clock::steady = 10;
  {
    Scope next(stream, other, space.Runtime());
    Clock::steady = 20;
    old.reset();
    auto active = Decode(stream.pending());
    AssertDecoded(active, 1);
    TEST_ASSERT_EQUAL_UINT8(255, active.records[0].duration_code);
    Clock::steady = 30;
  }
  auto completed = Decode(stream.pending()), saved = Decode(d.bytes);
  AssertDecoded(completed, 1);
  AssertDecoded(saved, 1);
  TEST_ASSERT_TRUE(completed.records[0].duration_state ==
                   DurationState::kValue);
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(20),
                          completed.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT8(255, saved.records[0].duration_code);
}
void test_exponential_time_uses_reconstructed_base_without_drift() {
  S space;
  space.SetRuntime({true, false, false, false, kTime});
  Destination d;
  Stream<S, 8192> stream(space, 1, Save, &d);
  for (unsigned i = 0; i != 1000; ++i)
    TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, space.Runtime(), {}, 0,
                                 1000 + i * 1000));
  TEST_ASSERT_TRUE(stream.Drain());
  auto decoded = Decode(d.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(1000, decoded.records.size());
  for (unsigned i = 0; i != 1000; ++i) {
    auto actual = std::uint64_t{1000} + i * 1000;
    TEST_ASSERT_TRUE(decoded.records[i].time.has_value());
    TEST_ASSERT_TRUE(*decoded.records[i].time <= actual);
    TEST_ASSERT_TRUE(actual - *decoded.records[i].time < 100);
  }
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 10));
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 100'000'000));
  TEST_ASSERT_TRUE(stream.Drain());
  decoded = Decode(d.bytes);
  AssertDecoded(decoded, 1002);
  TEST_ASSERT_TRUE(decoded.records[1000].time.has_value());
  TEST_ASSERT_TRUE(decoded.records[1001].time.has_value());
  TEST_ASSERT_EQUAL_UINT64(10, *decoded.records[1000].time);
  TEST_ASSERT_EQUAL_UINT64(100'000'000, *decoded.records[1001].time);
}
void test_runtime_off_fields_console_and_independent_spaces() {
  struct Second {};
  using App = Space<Second, Options{}, Clock>;
  constexpr Tag<App> app_point{7, 3, 2, "app", "app", __FILE__, __LINE__};
  S net;
  App app;
  Destination network, application;
  Stream<S, 128> ns(net, 1, Save, &network, Console, &network);
  Stream<App, 128> as(app, 1, Save, &application);
  net.SetRuntime({false, true, true, false, 0});
  app.SetRuntime({true, false, false, false, kTime});
  AE_RAW_LOG(ns, point);
  AE_RAW_LOG(as, app_point);
  TEST_ASSERT_TRUE(ns.pending().empty());
  TEST_ASSERT_TRUE(network.bytes.empty());
  TEST_ASSERT_EQUAL_UINT(1, Decode(network.console).records.size());
  net.SetRuntime({false, false, false, false, 0});
  int evaluated = 0;
  AE_RAW_PAYLOAD(ns, point, (++evaluated, std::span<std::uint8_t const>{}));
  TEST_ASSERT_EQUAL_INT(0, evaluated);
  TEST_ASSERT_TRUE(as.Drain());
  TEST_ASSERT_EQUAL_UINT(1, Decode(application.bytes).records.size());
  TEST_ASSERT_EQUAL_UINT64(1, ns.MetricFor(point.index).count);
}
void test_compile_off_has_no_clock_or_payload_evaluation() {
  struct NoClock {};
  using Off = Space<Identity, Options{false, false, false, false, 0}, NoClock>;
  constexpr Tag<Off> tag{7, 0, 0, "off", "off", __FILE__, __LINE__};
  Off space;
  Stream<Off, 128> stream(space, 1);
  int evaluated = 0;
  AE_RAW_PAYLOAD(stream, tag, (++evaluated, std::span<std::uint8_t const>{}));
  {
    AE_RAW_SCOPE(stream, tag);
  }
  TEST_ASSERT_EQUAL_INT(0, evaluated);
  TEST_ASSERT_TRUE(stream.pending().empty());
  TEST_ASSERT_EQUAL_UINT64(0, stream.MetricFor(point.index).count);
}
void test_embedded_recording_and_restore_do_not_allocate() {
  S space;
  space.SetRuntime({true, false, true, true, kTime | kDuration});
  allocations = 0;
  track_allocations = true;
  {
    Stream<S, 256> stream(space, 1, Discard);
    std::array seed{MetricTotal{point.index, {10, 1, 5, 5, 5}}};
    if (!stream.RestoreMetrics(seed)) std::abort();
    for (unsigned i = 0; i != 20; ++i) {
      Clock::steady = i * 10;
      Scope scope(stream, point, space.Runtime());
      Clock::steady += 5;
    }
    if (!stream.DrainMetrics()) std::abort();
  }
  track_allocations = false;
  TEST_ASSERT_EQUAL_UINT(0, allocations.load());
}
}  // namespace
void RunHealthy() {
  RUN_TEST(test_nested_duration_without_time_patches_each_reserved_byte);
  RUN_TEST(test_duration_ticket_cannot_patch_another_stream_instance);
  RUN_TEST(test_reconstruction_merges_streams_preserving_writer_order);
  RUN_TEST(test_duration_states_and_exact_endpoints);
  RUN_TEST(test_nested_flat_scopes_patch_one_entry_each);
  RUN_TEST(test_drain_invalidates_open_scope_without_completion_record);
  RUN_TEST(test_failed_drain_freezes_slot_and_retries_identical_bytes);
  RUN_TEST(test_generation_prevents_old_handle_patching_reused_offset);
  RUN_TEST(test_exponential_time_uses_reconstructed_base_without_drift);
  RUN_TEST(test_runtime_off_fields_console_and_independent_spaces);
  RUN_TEST(test_compile_off_has_no_clock_or_payload_evaluation);
  RUN_TEST(test_embedded_recording_and_restore_do_not_allocate);
}
