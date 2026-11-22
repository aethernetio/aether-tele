/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include <unity.h>
#include <array>
#include <vector>
#include "aether-tele/raw.h"
#include "aether-tele/raw/decode.h"

using namespace ae::tele::raw;
struct Identity {};
using S = Space<Identity>;
constexpr Tag<S> tag{7, 1, 1, "work", "work", __FILE__, __LINE__};

void test_record_has_only_tag_and_configured_data() {
  S space;
  Stream<S, 64> stream(space, 65535);
  TEST_ASSERT_TRUE(stream.Emit(
      tag, Kind::kLog, {true, false, false, false, kAllFields}, {}, 1, 1000));
  auto bytes = stream.pending();
  // An empty payload still has its length prefix. Writer width changes the
  // block header only; the record body remains exactly these four bytes.
  constexpr std::array<std::uint8_t, 4> expected{7, 0, 1, 0};
  TEST_ASSERT_EQUAL_MEMORY(expected.data(), bytes.data() + bytes.size() - 4, 4);
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(1, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT32(65535, decoded.records[0].stream);
  TEST_ASSERT_EQUAL_UINT64(1000, *decoded.records[0].time);
}

std::vector<std::uint8_t> TwoWriters() {
  std::array<std::uint8_t, 128> bytes{};
  std::size_t cursor = 0;
  TEST_ASSERT_TRUE(
      wire::WriteBlockPrefix(bytes, cursor, wire::kLogRecord, kTime, 2));
  for (auto id : {1u, 16u}) {
    TEST_ASSERT_TRUE(
        wire::WriteWriterHeader(bytes, cursor, kTime, id, 4, 2, id * 1000));
    // Two tag/time records per writer; no IDs or sequence numbers in them.
    for (unsigned item = 0; item < 2; ++item) {
      bytes[cursor++] = 7;
      bytes[cursor++] = 1;
    }
  }
  return {bytes.begin(), bytes.begin() + cursor};
}
void test_one_telemetry_block_contains_multiple_writers() {
  auto bytes = TwoWriters();
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(4, decoded.records.size());
  for (unsigned item = 0; item < 4; ++item) {
    auto id = item < 2 ? 1u : 16u;
    TEST_ASSERT_EQUAL_UINT32(id, decoded.records[item].stream);
    TEST_ASSERT_EQUAL_UINT64(id * 1000 + item % 2 + 1,
                             *decoded.records[item].time);
  }
  DecodeState state;
  auto first = Decode(bytes, state);
  auto second = Decode(bytes, state);
  TEST_ASSERT_TRUE(first.status == DecodeStatus::kOk &&
                   second.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT64(2,
                           second.records[0].sequence);  // derived ordinal only
}
void test_incomplete_writer_headers_and_bodies_do_not_commit_state() {
  auto bytes = TwoWriters();
  for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
    DecodeState state;
    state.time[99] = 123;
    auto result = Decode(std::span{bytes}.first(cut), state);
    TEST_ASSERT_TRUE(result.status != DecodeStatus::kOk);
    TEST_ASSERT_EQUAL_UINT(1, state.time.size());
    TEST_ASSERT_EQUAL_UINT64(123, state.time[99]);
    TEST_ASSERT_TRUE(state.ordinal.empty());
  }
}
void test_metric_values_choose_their_own_width_without_a_flag() {
  S space;
  std::vector<std::uint8_t> saved;
  auto append = +[](void* context, std::span<std::uint8_t const> bytes) {
    auto& destination = *static_cast<std::vector<std::uint8_t>*>(context);
    destination.insert(destination.end(), bytes.begin(), bytes.end());
    return true;
  };
  Stream<S, 64> stream(space, 1, append, &saved);
  for (unsigned i = 0; i < 201; ++i) stream.Count(7);
  stream.AddDuration(7, 100'000'001);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  // header 5; tag 1; count 2; samples 1; sum/min/max 8 each.
  TEST_ASSERT_EQUAL_UINT(33, saved.size());
  auto decoded = Decode(saved);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = RestoreMetric(decoded.records, 7);
  TEST_ASSERT_EQUAL_UINT64(201, metric.count);
  TEST_ASSERT_EQUAL_UINT64(100'000'001, metric.sum);
}

void test_header_growth_keeps_outstanding_duration_ticket_valid() {
  S space;
  Stream<S, 1024> stream(space, 1);
  Options options{true, false, false, false, kDuration};
  Stream<S, 1024>::DurationTicket ticket;
  TEST_ASSERT_TRUE(stream.Emit(tag, Kind::kLog, options, {}, 0, 0, &ticket));
  auto first_size = stream.pending().size();
  for (unsigned i = 0; i < 250; ++i)
    TEST_ASSERT_TRUE(stream.Emit(tag, Kind::kLog, options));
  // Both the body-length prefix and record-count prefix grew by one byte.
  TEST_ASSERT_EQUAL_UINT(first_size + 500 + 2, stream.pending().size());
  TEST_ASSERT_TRUE(stream.FinishDuration(ticket, 5));
  auto decoded = Decode(stream.pending());
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(251, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT8(wire::EncodeDuration(5),
                          decoded.records[0].duration_code);
}

void test_restore_rejects_out_of_range_metrics_atomically() {
  for (unsigned field = 0; field < 5; ++field) {
    S space;
    Stream<S, 64> stream(space, 1);
    Metric invalid{1, 1, 5, 5, 5};
    auto values = std::array{&invalid.count, &invalid.samples, &invalid.sum,
                             &invalid.min, &invalid.max};
    *values[field] = kMetricMax + 1;
    std::array seed{MetricTotal{7, invalid}};
    TEST_ASSERT_FALSE(stream.RestoreMetrics(seed));
    TEST_ASSERT_EQUAL_UINT64(0, stream.MetricFor(7).count);
    seed[0].metric = {1, 1, 5, 5, 5};
    TEST_ASSERT_TRUE(stream.RestoreMetrics(seed));
    TEST_ASSERT_EQUAL_UINT64(1, stream.MetricFor(7).count);
  }
}

void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_record_has_only_tag_and_configured_data);
  RUN_TEST(test_one_telemetry_block_contains_multiple_writers);
  RUN_TEST(test_incomplete_writer_headers_and_bodies_do_not_commit_state);
  RUN_TEST(test_metric_values_choose_their_own_width_without_a_flag);
  RUN_TEST(test_header_growth_keeps_outstanding_duration_ticket_valid);
  RUN_TEST(test_restore_rejects_out_of_range_metrics_atomically);
  return UNITY_END();
}
