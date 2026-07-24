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
#include <chrono>
#include <memory>
#include <thread>

namespace ae::tele::test_tele {
void test_TeleConfigurations() {
  {
    using Sink = TeleSink<tele_configuration::ConfigProvider<>>;
    auto tele_trap = std::make_shared<tele_configuration::TeleTrap>();
    Sink::Instance().SetTrap(tele_trap);
    {
      auto t =
          Tele<Sink, Sink::GetTeleConfig<Level::kDebug, ::Test.module.id>()>{
              Sink::Instance(),
              ::Test,
              Level{Level::kDebug},
              "test-tele.cpp",
              8,
              "message {}",
              12,
          };
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_.size());
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_[12].count_);
    TEST_ASSERT_GREATER_THAN(1, tele_trap->metric_data_[12].duration_);
    TEST_ASSERT_EQUAL(1, tele_trap->log_lines_.size());
    auto& log_line = tele_trap->log_lines_.front();
    TEST_ASSERT_EQUAL(7, log_line.size());
    TEST_ASSERT_EQUAL_STRING("12", log_line[0].c_str());
    AssertTimestampShape(log_line[1]);
    TEST_ASSERT_EQUAL_STRING("kDebug", log_line[2].c_str());
    TEST_ASSERT_EQUAL_STRING("TestObj", log_line[3].c_str());
    TEST_ASSERT_EQUAL_STRING("test-tele.cpp:8", log_line[4].c_str());
    TEST_ASSERT_EQUAL_STRING("Test", log_line[5].c_str());
    TEST_ASSERT_EQUAL_STRING("message 12", log_line[6].c_str());
  }
  {
    using Sink = TeleSink<tele_configuration::ConfigProvider<TeleConfig{
        .count_metrics = true,
        .time_metrics = true,
        .logs_enabled = false,
        .start_time_logs = false,
        .level_module_logs = false,
        .location_logs = false,
        .name_logs = false,
        .blob_logs = false,
    }>>;
    auto tele_trap = std::make_shared<tele_configuration::TeleTrap>();
    Sink::Instance().SetTrap(tele_trap);
    {
      auto t =
          Tele<Sink, Sink::GetTeleConfig<Level::kDebug, ::Test.module.id>()>{
              Sink::Instance(),
              ::Test,
              Level{Level::kDebug},
              "test-tele.cpp",
              8,
              "message {}",
              12,
          };
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_.size());
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_[12].count_);
    TEST_ASSERT_GREATER_THAN(1, tele_trap->metric_data_[12].duration_);
    TEST_ASSERT(tele_trap->log_lines_.empty());
  }
  {
    using Sink = TeleSink<tele_configuration::ConfigProvider<TeleConfig{
        .count_metrics = true,
        .time_metrics = false,
        .logs_enabled = false,
        .start_time_logs = false,
        .level_module_logs = false,
        .location_logs = false,
        .name_logs = false,
        .blob_logs = false,
    }>>;
    auto tele_trap = std::make_shared<tele_configuration::TeleTrap>();
    Sink::Instance().SetTrap(tele_trap);
    {
      auto t =
          Tele<Sink, Sink::GetTeleConfig<Level::kDebug, ::Test.module.id>()>{
              Sink::Instance(),
              ::Test,
              Level{Level::kDebug},
              "test-tele.cpp",
              8,
              "message {}",
              12,
          };
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_.size());
    TEST_ASSERT_EQUAL(1, tele_trap->metric_data_[12].count_);
    TEST_ASSERT_EQUAL(0, tele_trap->metric_data_[12].duration_);
    TEST_ASSERT(tele_trap->log_lines_.empty());
  }
  {
    using Sink = TeleSink<tele_configuration::ConfigProvider<TeleConfig{
        .count_metrics = false,
        .time_metrics = false,
        .logs_enabled = false,
        .start_time_logs = false,
        .level_module_logs = false,
        .location_logs = false,
        .name_logs = false,
        .blob_logs = false,
    }>>;
    auto tele_trap = std::make_shared<tele_configuration::TeleTrap>();
    Sink::Instance().SetTrap(tele_trap);
    {
      auto t =
          Tele<Sink, Sink::GetTeleConfig<Level::kDebug, ::Test.module.id>()>{
              Sink::Instance(),
              ::Test,
              Level{Level::kDebug},
              "test-tele.cpp",
              8,
              "message {}",
              12,
          };
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT(tele_trap->metric_data_.empty());
    TEST_ASSERT(tele_trap->log_lines_.empty());
  }
  {
    using Sink = TeleSink<tele_configuration::ConfigProvider<TeleConfig{
        .count_metrics = false,
        .time_metrics = false,
        .logs_enabled = true,
        .start_time_logs = false,
        .level_module_logs = true,
        .location_logs = false,
        .name_logs = true,
        .blob_logs = false,
    }>>;
    auto tele_trap = std::make_shared<tele_configuration::TeleTrap>();
    Sink::Instance().SetTrap(tele_trap);
    {
      auto t =
          Tele<Sink, Sink::GetTeleConfig<Level::kDebug, ::Test.module.id>()>{
              Sink::Instance(), Test, Level{Level::kDebug}, "test-tele.cpp", 8,
              "message {}",     12,
          };
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT(tele_trap->metric_data_.empty());
    TEST_ASSERT_EQUAL(1, tele_trap->log_lines_.size());
    auto& log_line = tele_trap->log_lines_.front();
    TEST_ASSERT_EQUAL(4, log_line.size());
    TEST_ASSERT_EQUAL_STRING("12", log_line[0].c_str());
    TEST_ASSERT_EQUAL_STRING("kDebug", log_line[1].c_str());
    TEST_ASSERT_EQUAL_STRING("TestObj", log_line[2].c_str());
    TEST_ASSERT_EQUAL_STRING("Test", log_line[3].c_str());
  }
}
}  // namespace ae::tele::test_tele
