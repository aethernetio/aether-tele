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
#include "compact_numeric.h"
#include "compact_regressions.h"

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

void test_block_size_golden_prefixes_and_atomic_truncation() {
  struct Boundary {
    std::size_t size, width;
    std::array<std::uint8_t, 4> prefix;
  };
  for (auto const& boundary :
       std::array{Boundary{250, 1, {0xfa}}, Boundary{251, 2, {0xfb, 0x00}},
                  Boundary{1500, 2, {0xff, 0xe1}},
                  Boundary{1501, 4, {0xff, 0xe2, 0x00, 0x00}}}) {
    std::vector<std::uint8_t> bytes(boundary.size, 7);
    std::copy_n(boundary.prefix.begin(), boundary.width, bytes.begin());
    auto cursor = boundary.width;
    bytes[cursor++] = 0;  // log, no optional fields
    bytes[cursor++] = 1;  // stream
    bytes[cursor++] = 0;
    bytes[cursor++] = 0;  // sequence
    auto const rows = bytes.size() - cursor;
    auto decoded = Decode(bytes);
    AssertDecoded(decoded, rows);
    TEST_ASSERT_EQUAL_UINT(bytes.size(), decoded.valid_bytes);
    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
      auto torn = Decode(std::span{bytes}.first(cut));
      TEST_ASSERT_TRUE(torn.status == DecodeStatus::kTruncated);
      TEST_ASSERT_TRUE(torn.records.empty());
      TEST_ASSERT_EQUAL_UINT(0, torn.valid_bytes);
    }
  }
  for (std::uint8_t size = 0; size < kMinBlockSize; ++size) {
    std::array prefix{size};
    TEST_ASSERT_TRUE(Decode(prefix).status == DecodeStatus::kMalformed);
  }
  TEST_ASSERT_EQUAL_UINT(wire::BlockSizeCodec::kUpper,
                         wire::BlockWireSize(wire::BlockSizeCodec::kUpper - 4));
  TEST_ASSERT_EQUAL_UINT(0,
                         wire::BlockWireSize(wire::BlockSizeCodec::kUpper - 3));
  TEST_ASSERT_EQUAL_UINT(
      0, wire::BlockWireSize(std::numeric_limits<std::size_t>::max()));
}

void test_prefix_growth_keeps_duration_slots_and_retry_bytes() {
  for (bool time : {false, true}) {
    S space;
    space.SetRuntime(
        {true, true, false, false, kDuration | kPayload | (time ? kTime : 0u)});
    Destination destination;
    Stream<S, 2048> stream(space, 1, Save, &destination, Console, &destination);
    using Ticket = Stream<S, 2048>::DurationTicket;
    Ticket early, middle, late;
    std::uint64_t timestamp = 1000;
    std::size_t rows = 0, middle_row = 0, late_row = 0;
    auto emit = [&](Ticket* ticket = nullptr) {
      std::array<std::uint8_t, 13> payload{};
      Store32(payload.data(), static_cast<std::uint32_t>(rows));
      auto const ok = stream.Emit(point, Kind::kLog, space.Runtime(),
                                  Data{payload}, 0, timestamp, ticket);
      timestamp += 1000;
      ++rows;
      return ok;
    };
    TEST_ASSERT_TRUE(emit(&early));
    while (!late.valid) {
      TEST_ASSERT_TRUE(emit());
      std::size_t width = 0;
      std::uint64_t size{};
      TEST_ASSERT_TRUE(
          wire::Read<wire::BlockSizeCodec>(stream.pending(), width, size) ==
          wire::ReadStatus::kOk);
      TEST_ASSERT_EQUAL_UINT(stream.pending().size(), size);
      if (width == 2 && !middle.valid) {
        middle_row = rows;
        TEST_ASSERT_TRUE(emit(&middle));
      }
      if (width == 4) {
        late_row = rows;
        TEST_ASSERT_TRUE(emit(&late));
      }
    }
    TEST_ASSERT_TRUE(destination.attempts.empty());
    TEST_ASSERT_TRUE(middle.valid);
    TEST_ASSERT_TRUE(stream.FinishDuration(early, 17));
    TEST_ASSERT_TRUE(stream.FinishDuration(middle, 100));
    auto decoded = Decode(stream.pending());
    AssertDecoded(decoded, rows);
    TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(17),
                            decoded.records[0].duration_code);
    TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(100),
                            decoded.records[middle_row].duration_code);
    TEST_ASSERT_EQUAL_UINT8(255, decoded.records[late_row].duration_code);
    auto console = Decode(destination.console);
    AssertDecoded(console, rows);
    TEST_ASSERT_EQUAL_UINT8(255, console.records[0].duration_code);
    TEST_ASSERT_EQUAL_UINT8(255, console.records[middle_row].duration_code);
    auto frozen = Copy(stream.pending());
    destination.accept = false;
    TEST_ASSERT_FALSE(stream.Drain());
    TEST_ASSERT_FALSE(stream.FinishDuration(late, 1000));
    TEST_ASSERT_FALSE(emit());
    TEST_ASSERT_TRUE(Copy(stream.pending()) == frozen);
    destination.accept = true;
    TEST_ASSERT_TRUE(stream.Drain());
    TEST_ASSERT_EQUAL_UINT(3, destination.attempts.size());
    for (auto const& attempt : destination.attempts)
      TEST_ASSERT_TRUE(attempt == frozen);
    TEST_ASSERT_TRUE(destination.bytes == frozen);
    TEST_ASSERT_TRUE(emit());
    TEST_ASSERT_FALSE(stream.FinishDuration(late, 1000));
  }
}

template <std::size_t Capacity>
void CheckPrefixCapacityDrain(std::size_t rows_per_block) {
  S space;
  space.SetRuntime({true, false, false, false, 0});
  Destination destination;
  Stream<S, Capacity> stream(space, 1, Save, &destination);
  auto tag = point;
  tag.index = 7;
  for (std::size_t i = 0; i <= rows_per_block; ++i)
    TEST_ASSERT_TRUE(stream.Emit(tag, Kind::kLog, space.Runtime()));
  TEST_ASSERT_EQUAL_UINT(Capacity, destination.bytes.size());
  TEST_ASSERT_EQUAL_UINT(6, stream.pending().size());
  TEST_ASSERT_TRUE(stream.Drain());
  auto decoded = Decode(destination.bytes);
  AssertDecoded(decoded, rows_per_block + 1);
  for (std::size_t i = 0; i < decoded.records.size(); ++i)
    TEST_ASSERT_EQUAL_UINT64(i, decoded.records[i].sequence);
  TEST_ASSERT_EQUAL_UINT64(0, stream.dropped());
}
void test_prefix_growth_respects_exact_capacity() {
  CheckPrefixCapacityDrain<250>(245);
  CheckPrefixCapacityDrain<1500>(1494);
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

void test_full_standalone_payload_rebases_when_delta_needs_two_bytes() {
  S space;
  space.SetRuntime({true, true, false, false, kTime | kPayload});
  Destination d;
  Stream<S, 80> stream(space, 1, Save, &d, Console, &d);
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, space.Runtime(), {}, 0, 1000));
  auto const overhead = stream.pending().size();
  std::vector<std::uint8_t> payload(80 - overhead, 0xab);
  auto const delta = wire::TimeValue(251);
  TEST_ASSERT_EQUAL_UINT(2,
                         wire::Size<wire::TimeCodec>(wire::EncodeTime(delta)));
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, space.Runtime(),
                               Data{payload}, 0, 1000 + delta));
  TEST_ASSERT_EQUAL_UINT(80, stream.pending().size());
  TEST_ASSERT_EQUAL_UINT(1, d.attempts.size());
  auto current = Decode(stream.pending());
  AssertDecoded(current, 1);
  TEST_ASSERT_TRUE(current.records[0].time.has_value());
  TEST_ASSERT_EQUAL_UINT64(1000 + delta, *current.records[0].time);
  TEST_ASSERT_EQUAL_UINT(payload.size(), current.records[0].payload.size());
  TEST_ASSERT_EQUAL_MEMORY(payload.data(), current.records[0].payload.data(),
                           payload.size());
  TEST_ASSERT_TRUE(stream.Drain());
  auto saved = Decode(d.bytes), console = Decode(d.console);
  AssertDecoded(saved, 2);
  AssertDecoded(console, 2);
  TEST_ASSERT_TRUE(saved.records[0].time.has_value());
  TEST_ASSERT_TRUE(saved.records[1].time.has_value());
  TEST_ASSERT_EQUAL_UINT64(1000, *saved.records[0].time);
  TEST_ASSERT_EQUAL_UINT64(1000 + delta, *saved.records[1].time);
  TEST_ASSERT_EQUAL_UINT(payload.size(), console.records[1].payload.size());
  TEST_ASSERT_EQUAL_MEMORY(payload.data(), console.records[1].payload.data(),
                           payload.size());
}

void test_torn_block_exposes_only_previous_complete_blocks() {
  S space;
  space.SetRuntime({true, false, false, false, kAllFields});
  Destination d;
  Stream<S, 128> stream(space, 1, Save, &d);
  std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, space.Runtime(), Data{payload}, 30, 1000));
  TEST_ASSERT_TRUE(
      stream.Emit(other, Kind::kLog, space.Runtime(), Data{payload}, 60, 2000));
  TEST_ASSERT_TRUE(stream.Drain());
  auto const boundary = d.bytes.size();
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, space.Runtime(), Data{payload}, 50, 3000));
  TEST_ASSERT_TRUE(stream.Drain());
  for (std::size_t cut = 1; cut < d.bytes.size(); ++cut) {
    auto partial = Decode(std::span{d.bytes}.first(cut));
    TEST_ASSERT_TRUE(
        partial.status ==
        (cut == boundary ? DecodeStatus::kOk : DecodeStatus::kTruncated));
    TEST_ASSERT_EQUAL_UINT(cut >= boundary ? boundary : 0, partial.valid_bytes);
    TEST_ASSERT_EQUAL_UINT(cut >= boundary ? 2 : 0, partial.records.size());
  }
  TEST_ASSERT_EQUAL_UINT(3, Decode(d.bytes).records.size());
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

void test_payload_is_captured_before_auto_drain_and_alias_release() {
  S space;
  space.SetRuntime({true, false, false, false, kPayload});
  Destination d;
  Stream<S, 128> stream(space, 1, Save, &d);
  std::array<std::uint8_t, 64> payload{};
  payload.fill(7);
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, space.Runtime(), Data{payload}));
  auto first = Copy(stream.pending());
  TEST_ASSERT_TRUE(
      stream.Emit(other, Kind::kLog, space.Runtime(), Data{stream.pending()}));
  TEST_ASSERT_TRUE(stream.Drain());
  auto decoded = Decode(d.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT(first.size(), decoded.records[1].payload.size());
  TEST_ASSERT_EQUAL_MEMORY(first.data(), decoded.records[1].payload.data(),
                           first.size());

  Destination mutation;
  Stream<S, 80> second(space, 2, Save, &mutation);
  TEST_ASSERT_TRUE(
      second.Emit(point, Kind::kLog, space.Runtime(), Data{payload}));
  mutation.mutate = &payload;
  TEST_ASSERT_TRUE(
      second.Emit(other, Kind::kLog, space.Runtime(), Data{payload}));
  TEST_ASSERT_TRUE(second.Drain());
  auto records = Decode(mutation.bytes);
  AssertDecoded(records, 2);
  for (auto byte : records.records[1].payload) TEST_ASSERT_EQUAL_UINT8(7, byte);
}

void test_field_changes_close_block_and_preserve_live_scope_snapshot() {
  S space;
  Destination d;
  Stream<S, 256> stream(space, 1, Save, &d);
  space.SetRuntime({true, false, false, false, kDuration});
  Clock::steady = 0;
  {
    Scope scope(stream, point, space.Runtime());
    space.SetRuntime({true, false, false, false, kTime});
    TEST_ASSERT_TRUE(
        stream.Emit(other, Kind::kLog, space.Runtime(), {}, 0, 5000));
    Clock::steady = 100;
  }
  TEST_ASSERT_TRUE(stream.Drain());
  auto records = Decode(d.bytes);
  TEST_ASSERT_TRUE(records.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, records.records.size());
  TEST_ASSERT_EQUAL_UINT32(kDuration, records.records[0].fields);
  TEST_ASSERT_EQUAL_UINT8(255, records.records[0].duration_code);
  TEST_ASSERT_EQUAL_UINT32(kTime, records.records[1].fields);
  TEST_ASSERT_TRUE(records.records[1].time.has_value());
  TEST_ASSERT_EQUAL_UINT64(5000, *records.records[1].time);
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

#if AE_TELE_TEST_NATIVE_FILE
struct TempTree {
  fs::path path;
  TempTree() {
    static std::atomic<unsigned> count{0};
    path = fs::temp_directory_path() /
           ("tele-compact-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(count++));
    if (!fs::create_directory(path)) throw std::runtime_error("mkdir fixture");
  }
  ~TempTree() {
    std::error_code ignored;
    if (path.parent_path() == fs::temp_directory_path())
      fs::remove_all(path, ignored);
  }
};
std::vector<std::uint8_t> ReadFile(fs::path const& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct FaultFile {
  static constexpr bool kDirectorySyncSupported =
      file_internal::NativeFile::kDirectorySyncSupported;
  file_internal::NativeFile native;
  bool fail_write{}, fail_sync{};
  bool Open(fs::path const& path, std::error_code& ec) {
    return native.Open(path, ec);
  }
  bool IsOpen() const { return native.IsOpen(); }
  bool Size(std::uint64_t& size, std::error_code& ec) {
    return native.Size(size, ec);
  }
  bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> out,
              std::size_t& count, std::error_code& ec) {
    return native.ReadAt(offset, out, count, ec);
  }
  bool WriteAt(std::uint64_t offset, std::span<std::uint8_t const> bytes,
               std::size_t& count, std::error_code& ec) {
    if (fail_write) {
      fail_write = false;
      if (!native.WriteAt(offset,
                          bytes.first(std::min<std::size_t>(5, bytes.size())),
                          count, ec))
        return false;
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    return native.WriteAt(offset, bytes, count, ec);
  }
  bool Sync(std::error_code& ec) {
    if (fail_sync) {
      fail_sync = false;
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    return native.Sync(ec);
  }
  bool Truncate(std::uint64_t size, std::error_code& ec) {
    return native.Truncate(size, ec);
  }
  bool SyncParent(std::error_code& ec) { return native.SyncParent(ec); }
  bool Close(std::error_code& ec) { return native.Close(ec); }
};
struct CapturingFile {
  BasicFileDrain<FaultFile>& sink;
  std::vector<std::vector<std::uint8_t>> attempts{};
  static bool Callback(void* context, std::span<std::uint8_t const> bytes) {
    auto& d = *static_cast<CapturingFile*>(context);
    d.attempts.emplace_back(bytes.begin(), bytes.end());
    return d.sink.Append(bytes);
  }
};

void test_native_restore_and_metric_retry_across_multiple_reopens() {
  for (bool sync : {false, true}) {
    TempTree tree;
    auto path = tree.path / "metrics.raw";
    for (unsigned boot = 0; boot != 4; ++boot) {
      FaultFile backend;
      BasicFileDrain<FaultFile> sink(backend);
      TEST_ASSERT_TRUE(sink.Open(path));
      S space;
      CapturingFile d{sink};
      Stream<S, 128> stream(space, 3, CapturingFile::Callback, &d);
      TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
      AssertMetric(
          boot ? Metric{2u * boot, 2u * boot, 12u * boot, 5, 7} : Metric{},
          stream.MetricFor(point.index));
      auto checkpoint = sink.size();
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      TEST_ASSERT_EQUAL_UINT64(checkpoint, sink.size());
      stream.Count(point.index);
      stream.AddDuration(point.index, 5);
      if (sync)
        backend.fail_sync = true;
      else
        backend.fail_write = true;
      TEST_ASSERT_FALSE(stream.DrainMetrics());
      TEST_ASSERT_EQUAL_UINT64(checkpoint, sink.size());
      stream.Count(point.index);
      stream.AddDuration(point.index, 7);
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      TEST_ASSERT_TRUE(d.attempts.size() >= 2);
      TEST_ASSERT_TRUE(d.attempts[0] == d.attempts[1]);
      AssertMetric({2u * (boot + 1), 2u * (boot + 1), 12u * (boot + 1), 5, 7},
                   stream.MetricFor(point.index));
      TEST_ASSERT_TRUE(sink.Close());
    }
  }
}

void test_wide_metric_values_restore_without_range_loss() {
  std::array<std::uint8_t, 128> bytes{};
  std::size_t cursor = 5;
  bytes[4] = 0x81;
  TEST_ASSERT_TRUE(wire::Write<wire::StreamCodec>(3, bytes, cursor));
  TEST_ASSERT_TRUE(wire::WriteU64(0, bytes, cursor));
  TEST_ASSERT_TRUE(wire::Write<wire::TagCodec>(point.index, bytes, cursor));
  auto maximum = std::numeric_limits<std::uint64_t>::max();
  for (auto value :
       {maximum, maximum, maximum, std::uint64_t{1}, std::uint64_t{2}})
    TEST_ASSERT_TRUE(wire::WriteU64(value, bytes, cursor));
  auto const block = wire::FinishBlock(bytes, cursor);
  TempTree tree;
  FileDrain sink;
  TEST_ASSERT_TRUE(sink.Open(tree.path / "wide.raw"));
  TEST_ASSERT_TRUE(sink.Append(block));
  S space;
  Stream<S, 128> stream(space, 3, FileDrain::Callback, &sink);
  TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
  AssertMetric({maximum, maximum, maximum, 1, 2},
               stream.MetricFor(point.index));
  stream.Count(point.index);
  stream.AddDuration(point.index, 5);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  TEST_ASSERT_TRUE(sink.Close());
  auto saved = ReadFile(tree.path / "wide.raw");
  AssertMetric({maximum, maximum, maximum, 1, 5},
               RestoreMetric(Decode(saved).records, point.index));
}

void test_shared_native_sink_restores_and_serializes_four_writers() {
  TempTree tree;
  FileDrain sink;
  TEST_ASSERT_TRUE(sink.Open(tree.path / "threads.raw"));
  struct Threaded {};
  using T = Space<Threaded>;
  constexpr Tag<T> tag{7, 3, 2, "work", "thread work", __FILE__, __LINE__};
  T space;
  std::array<std::thread, 4> threads;
  std::atomic<bool> failed{false};
  for (std::uint32_t id = 0; id != threads.size(); ++id)
    threads[id] = std::thread([&, id] {
      Stream<T, 128> stream(space, id, FileDrain::Callback, &sink);
      if (!sink.RestoreMetrics(stream)) failed = true;
      for (unsigned i = 0; i != 30; ++i) {
        stream.Count(point.index);
        stream.AddDuration(point.index, id + 1);
        if (!stream.Emit(tag, Kind::kLog, {true, false, false, false, kTime},
                         {}, 0, 1000 + i))
          failed = true;
        if (i % 5 == 0 && !stream.DrainMetrics()) failed = true;
      }
      if (!stream.DrainMetrics()) failed = true;
    });
  for (auto& t : threads) t.join();
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.Close());
  TEST_ASSERT_TRUE(sink.Open(tree.path / "threads.raw"));
  for (std::uint32_t id = 0; id != threads.size(); ++id) {
    Stream<T, 128> stream(space, id);
    TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
    AssertMetric({30, 30, 30u * (id + 1), id + 1, id + 1},
                 stream.MetricFor(point.index));
  }
}

Metric BootMetric(unsigned boot, bool second) {
  if (boot == 0) return {};
  if (second)
    return {boot, boot, 100u * boot + boot * (boot - 1u) / 2u, 100, 99u + boot};
  return {2u * boot, 2u * boot, 12u * boot, 5, 7};
}
struct RejectOnce {
  FileDrain& sink;
  bool fail{};
  std::vector<std::uint8_t> rejected{};
  bool same{true};
  static bool Callback(void* context, std::span<std::uint8_t const> bytes) {
    auto& d = *static_cast<RejectOnce*>(context);
    if (d.fail) {
      d.fail = false;
      d.rejected = Copy(bytes);
      return false;
    }
    if (!d.rejected.empty()) {
      d.same = d.same && d.rejected == Copy(bytes);
      d.rejected.clear();
    }
    return d.sink.Append(bytes);
  }
};
#endif
int Process(std::string const& mode, fs::path const& path, unsigned boot) {
  if (mode == "--fixture") {
    S space;
    space.SetRuntime({true, false, false, false, kTime | kDuration | kPayload});
    Destination destination;
    Stream<S, 4096> stream(space, 3, Save, &destination);
    std::array<std::uint8_t, 3> payload{0, 128, 255};
    if (!stream.Emit(point, Kind::kLog, space.Runtime(), Data{payload}, 10,
                     1000) ||
        !stream.Emit(point, Kind::kLog, space.Runtime(), {}, 100, 2000) ||
        !stream.Emit(other, Kind::kLog, space.Runtime(), {}, 1000, 9000) ||
        !stream.Drain())
      return 1;
    for (auto size : {251u, 1501u}) {
      std::vector<std::uint8_t> blob(size, 0xa5);
      if (!stream.Emit(point, Kind::kLog, space.Runtime(), Data{blob}, 1000,
                       10000 + size) ||
          !stream.Drain())
        return 1;
    }
    std::ofstream capture(path, std::ios::binary);
    capture.write(reinterpret_cast<char const*>(destination.bytes.data()),
                  destination.bytes.size());
    return capture ? 0 : 1;
  }
  if (mode == "--map") {
    std::ofstream map(path);
    std::array tags{point, other, start};
    return ExportMap(map, "compact-tests", "compact-v1",
                     std::span<Tag<S> const>{tags})
               ? 0
               : 1;
  }
#if AE_TELE_TEST_NATIVE_FILE
  FileDrain sink;
  if (!sink.Open(path)) return 2;
  S space;
  RejectOnce callback{sink};
  Stream<S, 128> first(space, 3, RejectOnce::Callback, &callback);
  Stream<S, 128> second(space, 9, FileDrain::Callback, &sink);
  if (!sink.RestoreMetrics(first) || !sink.RestoreMetrics(second)) return 3;
  if (!SameMetric(first.MetricFor(point.index), BootMetric(boot, false)) ||
      !SameMetric(second.MetricFor(point.index), BootMetric(boot, true)))
    return 4;
  auto checkpoint = sink.size();
  if (!first.DrainMetrics() || !second.DrainMetrics() ||
      sink.size() != checkpoint)
    return 5;
  if (mode == "--verify") return sink.Close() ? 0 : 6;
  if (mode != "--boot") return 7;
  first.Count(point.index);
  first.AddDuration(point.index, 5);
  callback.fail = true;
  if (first.DrainMetrics()) return 8;
  first.Count(point.index);
  first.AddDuration(point.index, 7);
  if (!first.DrainMetrics() || !callback.same) return 9;
  second.Count(point.index);
  second.AddDuration(point.index, 100u + boot);
  if (!second.DrainMetrics()) return 10;
  if (!first.Emit(start, Kind::kLog, {true, false, false, false, kTime}, {}, 0,
                  1000u + boot * 100u) ||
      !first.Drain())
    return 11;
  std::_Exit(0);  // No stop, Close or destructors.
#else
  static_cast<void>(boot);
  return 7;
#endif
}
}  // namespace

void setUp() {
  Clock::time = 1000;
  Clock::steady = 0;
  Clock::reads = 0;
}
void tearDown() { track_allocations = false; }
int main(int argc, char** argv) {
  if (argc >= 3)
    return Process(argv[1], argv[2],
                   argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 0);
  UNITY_BEGIN();
  RUN_TEST(test_block_size_golden_prefixes_and_atomic_truncation);
  RUN_TEST(test_prefix_growth_keeps_duration_slots_and_retry_bytes);
  RUN_TEST(test_prefix_growth_respects_exact_capacity);
  RUN_TEST(test_nested_duration_without_time_patches_each_reserved_byte);
  RUN_TEST(test_duration_ticket_cannot_patch_another_stream_instance);
  RUN_TEST(test_full_standalone_payload_rebases_when_delta_needs_two_bytes);
  RUN_TEST(test_torn_block_exposes_only_previous_complete_blocks);
  RUN_TEST(test_reconstruction_merges_streams_preserving_writer_order);
  compact_regressions::RunTests();
  compact_numeric_tests::Run();
  RUN_TEST(test_duration_states_and_exact_endpoints);
  RUN_TEST(test_nested_flat_scopes_patch_one_entry_each);
  RUN_TEST(test_drain_invalidates_open_scope_without_completion_record);
  RUN_TEST(test_failed_drain_freezes_slot_and_retries_identical_bytes);
  RUN_TEST(test_generation_prevents_old_handle_patching_reused_offset);
  RUN_TEST(test_payload_is_captured_before_auto_drain_and_alias_release);
  RUN_TEST(test_field_changes_close_block_and_preserve_live_scope_snapshot);
  RUN_TEST(test_exponential_time_uses_reconstructed_base_without_drift);
  RUN_TEST(test_runtime_off_fields_console_and_independent_spaces);
  RUN_TEST(test_compile_off_has_no_clock_or_payload_evaluation);
  RUN_TEST(test_embedded_recording_and_restore_do_not_allocate);
#if AE_TELE_TEST_NATIVE_FILE
  RUN_TEST(test_native_restore_and_metric_retry_across_multiple_reopens);
  RUN_TEST(test_wide_metric_values_restore_without_range_loss);
  RUN_TEST(test_shared_native_sink_restores_and_serializes_four_writers);
#endif
  return UNITY_END();
}
