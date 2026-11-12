/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
#include "aether-tele/traps/statistics_trap.h"
#include <unity.h>
#include <atomic>
#include <thread>

namespace {
inline constexpr ae::tele::Module module{1, 10, 20, "module"};
inline constexpr ae::tele::Tag point{0, module, "point"};
class ByteCollector final : public ae::tele::ILogCollector {
 public:
  void WriteLine(ae::tele::ILogLine& line) override {
    std::uint8_t const value = 0x55;
    line.Blob({&value, 1});
  }
};
void test_merge_does_not_lose_concurrent_destination_updates() {
  ae::tele::StatisticsTrap<8192> source;
  ae::tele::StatisticsTrap<8192> destination;
  ByteCollector collector;
  source.AddInvoke(point, 123);
  source.LogLine(point, collector);
  std::atomic<bool> start{false};
  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire)) {}
    for (unsigned i = 0; i != 2000; ++i) {
      destination.AddInvoke(point, 1);
      destination.LogLine(point, collector);
    }
  });
  start.store(true, std::memory_order_release);
  for (unsigned i = 0; i != 100; ++i) destination.MergeStatistics(source);
  writer.join();
  auto const count = static_cast<std::uint64_t>(
      destination.metrics_store().metrics.at(point.index()).invocations_count);
  TEST_ASSERT_TRUE(count == 2000 + 100 * 123);
  TEST_ASSERT_EQUAL_UINT(4200, destination.log_storage().size());
  auto before = destination.log_storage().size();
  destination.MergeStatistics(destination);
  TEST_ASSERT_EQUAL_UINT(before, destination.log_storage().size());
}
void test_merge_synchronizes_source_environment_and_metrics() {
  ae::tele::StatisticsTrap<8192> source;
  ae::tele::StatisticsTrap<8192> destination;
  std::atomic<bool> start{false};
  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire)) {}
    for (unsigned i = 0; i != 1000; ++i) {
      source.AddInvoke(point, 1);
      ae::tele::EnvData environment{};
      environment.platform_type = i % 2 ? "short" : "a much longer platform value";
      source.WriteEnvData(environment);
    }
  });
  start.store(true, std::memory_order_release);
  for (unsigned i = 0; i != 1000; ++i) destination.MergeStatistics(source);
  writer.join();
  destination.MergeStatistics(source);
  TEST_ASSERT_EQUAL_STRING("short", destination.env_store().platform.c_str());
  TEST_ASSERT_TRUE(static_cast<std::uint64_t>(
      destination.metrics_store().metrics.at(point.index()).invocations_count) >= 1000);
}
void test_opposite_merges_do_not_deadlock() {
  ae::tele::StatisticsTrap<256> a;
  ae::tele::StatisticsTrap<256> b;
  a.AddInvoke(point, 1); b.AddInvoke(point, 1);
  std::thread first([&] { for (unsigned i = 0; i != 5; ++i) a.MergeStatistics(b); });
  std::thread second([&] { for (unsigned i = 0; i != 5; ++i) b.MergeStatistics(a); });
  first.join(); second.join();
  TEST_ASSERT_TRUE(!a.metrics_store().metrics.empty());
  TEST_ASSERT_TRUE(!b.metrics_store().metrics.empty());
}
}
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_merge_does_not_lose_concurrent_destination_updates);
  RUN_TEST(test_merge_synchronizes_source_environment_and_metrics);
  RUN_TEST(test_opposite_merges_do_not_deadlock);
  return UNITY_END();
}
