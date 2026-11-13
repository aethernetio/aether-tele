/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include <unity.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include "aether-tele/raw.h"
#include "aether-tele/raw/file.h"
using namespace ae::tele::raw;
struct FileIdentity {};
using S = Space<FileIdentity>;
constexpr Tag<S> tag{7, 1, 1, "file", "file", __FILE__, __LINE__};
void test_file_reopen_restores_metrics_and_appends_flat_records() {
  auto path =
      std::filesystem::temp_directory_path() /
      ("tele-flat-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  for (unsigned boot = 0; boot < 4; ++boot) {
    FileDrain file;
    TEST_ASSERT_TRUE(file.Open(path));
    S space;
    Stream<S, 64> stream(space, 3, FileDrain::Callback, &file);
    TEST_ASSERT_TRUE(file.RestoreMetrics(stream));
    TEST_ASSERT_TRUE(stream.MetricFor(7).count == boot);
    TEST_ASSERT_TRUE(stream.Emit(
        tag, Kind::kLog, {true, false, true, true, kTime}, {}, 0, 1000 + boot));
    TEST_ASSERT_TRUE(stream.Drain());
    TEST_ASSERT_TRUE(stream.Emit(
        tag, Kind::kLog, {true, false, true, true, kTime}, {}, 0, 1001 + boot));
    stream.Count(7);
    stream.AddDuration(7, 5);
    TEST_ASSERT_TRUE(stream.DrainMetrics());
    TEST_ASSERT_TRUE(file.Close());
  }
  std::ifstream in(path, std::ios::binary);
  std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
  auto d = Decode(bytes);
  TEST_ASSERT_TRUE(d.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(12, d.records.size());
  TEST_ASSERT_TRUE(RestoreMetric(d.records, 7).count == 4);
  for (unsigned boot = 0; boot < 4; ++boot) {
    TEST_ASSERT_TRUE(*d.records[3 * boot].time == 1000 + boot);
    TEST_ASSERT_TRUE(*d.records[3 * boot + 1].time == 1001 + boot);
  }
  in.close();
  std::filesystem::remove(path);
}
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_file_reopen_restores_metrics_and_appends_flat_records);
  return UNITY_END();
}
