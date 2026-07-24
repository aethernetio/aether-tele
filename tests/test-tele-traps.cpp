/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tele_test_common.h"

#include <unity.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <thread>

namespace ae::tele::test_tele {
void test_TeleProxyTrap() {
  using ProxyTeleTrap =
      ProxyTrap<tele_configuration::TeleTrap, tele_configuration::TeleTrap>;
  auto first_trap = std::make_shared<tele_configuration::TeleTrap>();
  auto second_trap = std::make_shared<tele_configuration::TeleTrap>();
  auto proxy_tele_trap =
      std::make_shared<ProxyTeleTrap>(first_trap, second_trap);
  SinkType::Instance().SetTrap(proxy_tele_trap);
  {
    AE_TELE_DEBUG(::Test1);
    AE_TELE_DEBUG(::Test1, "format {}", 12);
    AE_TELE_INFO(::Test2, "format {}", 24);
    AE_TELE_INFO(::Test2, "format {}", 48);
  }
  auto& metrics =
      first_trap->metric_data_[::Test1.offset + ::TestObj.index_start];
  TEST_ASSERT_EQUAL(2, metrics.count_);
  auto d = second_trap->metric_data_[::Test1.offset + ::TestObj.index_start];
  TEST_ASSERT_EQUAL(metrics.count_, d.count_);
}

void test_MergeStatisticsTrap() {
  using Sink = TeleSink<tele_configuration::ConfigProvider<TeleConfig{
      .count_metrics = true,
      .time_metrics = true,
      .logs_enabled = true,
      .start_time_logs = false,
      .level_module_logs = false,
      .location_logs = false,
      .name_logs = false,
      .blob_logs = false,
  }>>;
#undef TELE_SINK
#define TELE_SINK Sink
  using Trap = StatisticsTrap<1024>;
  auto statistics_trap1 = std::make_shared<Trap>();
  Sink::Instance().SetTrap(statistics_trap1);
  {
    AE_TELE_DEBUG(::Test1);
    AE_TELE_INFO(::Test1);
    AE_TELE_WARNING(::Test1);
    AE_TELE_ERROR(::Test1);
    AE_TELE_DEBUG(::Test2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto const& logs1 = statistics_trap1->log_storage();
  auto statistics_trap2 = std::make_shared<Trap>();
  statistics_trap2->MergeStatistics(*statistics_trap1);
  auto const& logs2 = statistics_trap2->log_storage();
  TEST_ASSERT_EQUAL(logs1.size(), logs2.size());
  TEST_ASSERT_EQUAL_CHAR_ARRAY(logs1.buffer.data(), logs2.buffer.data(),
                               logs2.buffer.size());
  auto const& metrics1 = statistics_trap1->metrics_store().metrics;
  auto const& metrics2 = statistics_trap2->metrics_store().metrics;
  TEST_ASSERT_EQUAL(metrics1.size(), metrics2.size());
  auto mit1 = std::begin(metrics1);
  auto mit2 = std::begin(metrics2);
  for (; mit1 != std::end(metrics1); ++mit1, ++mit2) {
    TEST_ASSERT_EQUAL(mit1->first, mit2->first);
    TEST_ASSERT_EQUAL(mit1->second.invocations_count,
                      mit2->second.invocations_count);
    TEST_ASSERT_EQUAL(mit1->second.max_duration, mit2->second.max_duration);
    TEST_ASSERT_EQUAL(mit1->second.min_duration, mit2->second.min_duration);
    TEST_ASSERT_EQUAL(mit1->second.sum_duration, mit2->second.sum_duration);
  }
  Sink::Instance().SetTrap(statistics_trap2);
  {
    AE_TELE_DEBUG(::Test1);
    AE_TELE_DEBUG(::Test2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  TEST_ASSERT_NOT_EQUAL(logs1.size(), logs2.size());
  TEST_ASSERT_EQUAL(metrics1.size(), metrics2.size());
  auto new_mit1 = std::begin(metrics1);
  auto new_mit2 = std::begin(metrics2);
  for (; new_mit1 != std::end(metrics1); ++new_mit1, ++new_mit2) {
    TEST_ASSERT_EQUAL(new_mit1->first, new_mit2->first);
    TEST_ASSERT_NOT_EQUAL(new_mit1->second.invocations_count,
                          new_mit2->second.invocations_count);
    TEST_ASSERT_NOT_EQUAL(new_mit1->second.sum_duration,
                          new_mit2->second.sum_duration);
  }
#undef TELE_SINK
#define TELE_SINK SinkType
}

void test_StatisticsRotation() {
  using Trap = ae::tele::StatisticsTrap<1024>;
  auto ts = std::make_shared<Trap>();
  SinkType::Instance().SetTrap(ts);
  TEST_ASSERT_EQUAL(0, ts->log_storage().size());
  {
    AE_TELE_DEBUG(::Test1, "12");
  }
  TEST_ASSERT_GREATER_THAN(0, ts->log_storage().size());
  std::array<std::uint8_t, 800> garbage{};
  {
    AE_TELE_DEBUG(::Test1, "13 {}", garbage);
  }
  TEST_ASSERT_EQUAL(1023, ts->log_storage().size());
}

void test_SaveLoadTeleStatistics() {
  using Trap = ae::tele::StatisticsTrap<1024>;
  auto ts_0 = std::make_shared<Trap>();
  auto ts_1 = std::make_shared<Trap>();
  SinkType::Instance().SetTrap(ts_0);
  {
    AE_TELE_DEBUG(::Test1, "12");
  }
  auto size_before = ts_0->log_storage().size();
  ts_1->MergeStatistics(*ts_0);
  auto size_after_merge = ts_1->log_storage().size();
  TEST_ASSERT_EQUAL(size_before, size_after_merge);
  auto const& metrics1 = ts_0->metrics_store().metrics;
  auto const& metrics2 = ts_1->metrics_store().metrics;
  TEST_ASSERT_EQUAL(metrics1.size(), metrics2.size());
  auto log_index = ae::tele::MetricsStore::PackedIndex{::TestObj.index_start +
                                                       ::Test1.offset};
  if constexpr (TELE_SINK::GetTeleConfig<Level::kDebug, MLog.id>()
                    .time_metrics) {
    TEST_ASSERT_EQUAL(metrics1.at(log_index).sum_duration,
                      metrics2.at(log_index).sum_duration);
  }
  if constexpr (TELE_SINK::GetTeleConfig<Level::kDebug, MLog.id>()
                    .count_metrics) {
    TEST_ASSERT_EQUAL(metrics1.at(log_index).invocations_count,
                      metrics2.at(log_index).invocations_count);
  }
  SinkType::Instance().SetTrap(ts_1);
  {
    AE_TELE_DEBUG(::Test1, "13");
  }
  TEST_ASSERT_GREATER_THAN(size_before, ts_1->log_storage().size());
}
}  // namespace ae::tele::test_tele
