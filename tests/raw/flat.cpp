/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include <unity.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "aether-tele/raw.h"
#include "aether-tele/raw/decode.h"
#include "compact_numeric.h"
#include "compact_regressions.h"
#if AE_TELE_TEST_NATIVE_FILE
#  include "aether-tele/raw/file.h"
#endif
using namespace ae::tele::raw;
struct Identity {};
using TestSpace = Space<Identity>;
#ifndef RAW_TEST_TAG_INDEX
#  define RAW_TEST_TAG_INDEX 7
#endif
AE_RAW_TAG(TestSpace, point, RAW_TEST_TAG_INDEX, 1, 1, "point");
struct Output {
  std::vector<std::uint8_t> bytes;
  static bool Save(void* p, std::span<std::uint8_t const> value) {
    auto& self = *static_cast<Output*>(p);
    self.bytes.insert(self.bytes.end(), value.begin(), value.end());
    return true;
  }
};
std::uint64_t FirstTag(std::span<std::uint8_t const> bytes) {
  std::size_t cursor = 0;
  std::uint64_t index = 0;
  TEST_ASSERT_TRUE(wire::Read<wire::TagCodec>(bytes, cursor, index) ==
                   wire::ReadStatus::kOk);
  return index;
}
void test_flat_anchor_and_continuous_drain() {
  TestSpace space;
  Output output;
  Stream<TestSpace, 64> stream(space, 3, Output::Save, &output);
  Options o{true, false, true, true, kTime};
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, o, {}, 0, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, stream.pending()[0]);
  TEST_ASSERT_TRUE(stream.Drain());
  auto boundary = output.bytes.size();
  TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, o, {}, 0, 1001));
  TEST_ASSERT_EQUAL_UINT(point.index, FirstTag(stream.pending()));
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL_UINT(point.index,
                         FirstTag(std::span{output.bytes}.subspan(boundary)));
  auto decoded = Decode(output.bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, decoded.records.size());
  TEST_ASSERT_TRUE(*decoded.records[0].time == 1000);
  TEST_ASSERT_TRUE(*decoded.records[1].time == 1001);
  DecodeState incremental;
  TEST_ASSERT_TRUE(
      Decode(std::span{output.bytes}.first(boundary), incremental).status ==
      DecodeStatus::kOk);
  TEST_ASSERT_TRUE(
      *Decode(std::span{output.bytes}.subspan(boundary), incremental)
           .records[0]
           .time == 1001);
}
void test_pause_gap_and_restart() {
  TestSpace space;
  Output output;
  Options o{true, false, true, true, kTime};
  for (auto time : {1000ULL, 100000000ULL, 50ULL}) {
    Stream<TestSpace, 64> stream(space, 3, Output::Save, &output);
    TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, o, {}, 0, time));
    TEST_ASSERT_TRUE(stream.Drain());
    space.SetRuntime({false, false, false, false, 0});
    space.SetRuntime(o);
    TEST_ASSERT_TRUE(stream.Emit(point, Kind::kLog, o, {}, 0, time + 1));
    TEST_ASSERT_EQUAL_UINT8(0, stream.pending()[0]);
    TEST_ASSERT_TRUE(stream.Drain());
  }
  auto d = Decode(output.bytes);
  TEST_ASSERT_TRUE(d.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(6, d.records.size());
  TEST_ASSERT_TRUE(*d.records[2].time == 100000000);
  TEST_ASSERT_TRUE(*d.records[4].time == 50);
}
void test_duration_metrics_and_writer_interleave() {
  TestSpace a, b;
  Output output;
  Stream<TestSpace, 64> first(a, 1, Output::Save, &output);
  Stream<TestSpace, 64> second(b, 2, Output::Save, &output);
  Options o{true, false, true, true, kTime | kDuration};
  Stream<TestSpace, 64>::DurationTicket ticket;
  TEST_ASSERT_TRUE(first.Emit(point, Kind::kLog, o, {}, 0, 100, &ticket));
  TEST_ASSERT_TRUE(first.FinishDuration(ticket, 1));
  TEST_ASSERT_TRUE(first.Drain());
  TEST_ASSERT_FALSE(first.FinishDuration(ticket, 5));
  TEST_ASSERT_TRUE(second.Emit(point, Kind::kLog, o, {}, 0, 200));
  TEST_ASSERT_TRUE(second.Drain());
  TEST_ASSERT_TRUE(first.Emit(point, Kind::kLog, o, {}, 0, 101));
  first.Count(point.index);
  first.AddDuration(point.index, 9);
  TEST_ASSERT_TRUE(first.DrainMetrics());
  auto d = Decode(output.bytes);
  TEST_ASSERT_TRUE(d.status == DecodeStatus::kOk);
  TEST_ASSERT_TRUE(*d.records[0].duration == 1);
  TEST_ASSERT_TRUE(*d.records[2].time == 101);
  TEST_ASSERT_TRUE(RestoreMetric(d.records, point.index).count == 1);
}
void test_zero_is_reserved_and_no_fields() {
  TestSpace space;
  Output output;
  Stream<TestSpace, 64> stream(space, 1, Output::Save, &output);
  auto zero = point;
  zero.index = 0;
  TEST_ASSERT_FALSE(stream.Emit(zero, Kind::kLog, space.Runtime()));
  TEST_ASSERT_TRUE(
      stream.Emit(point, Kind::kLog, {true, false, false, false, 0}));
  TEST_ASSERT_EQUAL_UINT(point.index, FirstTag(stream.pending()));
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_EQUAL_UINT(1, Decode(output.bytes).records.size());
}
int Process(std::string mode, std::filesystem::path path, unsigned boot) {
  if (mode == "--map") {
    std::ofstream out(path);
    Tag<TestSpace> tags[]{point};
    return ExportMap(out, "flat-test", "test",
                     std::span<Tag<TestSpace> const>{tags})
               ? 0
               : 1;
  }
  Output out;
  if (std::filesystem::exists(path)) {
    std::ifstream in(path, std::ios::binary);
    out.bytes.assign(std::istreambuf_iterator<char>{in}, {});
  }
  TestSpace space;
  Stream<TestSpace, 4096> stream(space, 3, Output::Save, &out);
  auto before = Decode(out.bytes);
  if (before.status != DecodeStatus::kOk) return 2;
  auto metric = RestoreMetric(before.records, point.index);
  MetricTotal totals[]{{point.index, metric}};
  if (!stream.RestoreMetrics(totals)) return 3;
  if (mode == "--verify")
    return stream.MetricFor(point.index).count == boot ? 0 : 4;
  if (mode == "--fixture") out.bytes.clear();
  if (!stream.Emit(point, Kind::kLog,
                   {true, false, true, true, kTime | kDuration}, {}, 1,
                   1000 + boot))
    return 5;
  stream.Count(point.index);
  stream.AddDuration(point.index, 5);
  if (!stream.DrainMetrics()) return 6;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<char const*>(out.bytes.data()), out.bytes.size());
  return file ? 0 : 7;
}
void RunHealthy();
void setUp() {}
void tearDown() {}
int main(int argc, char** argv) {
  if (argc >= 3)
    return Process(argv[1], argv[2], argc > 3 ? std::stoul(argv[3]) : 0);
  UNITY_BEGIN();
  RUN_TEST(test_flat_anchor_and_continuous_drain);
  RUN_TEST(test_pause_gap_and_restart);
  RUN_TEST(test_duration_metrics_and_writer_interleave);
  RUN_TEST(test_zero_is_reserved_and_no_fields);
  RunHealthy();
  compact_numeric_tests::Run();
  compact_regressions::RunTests();
  return UNITY_END();
}
