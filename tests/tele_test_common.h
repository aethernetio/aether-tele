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

#ifndef AE_TELE_TEST_COMMON_H_
#define AE_TELE_TEST_COMMON_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define TELE_SINK SinkType
#include "aether-tele/tele.h"
#include "tele_test_tags.h"

using SinkType = ae::tele::TeleSink<ae::tele::AllEnabledConfig>;

namespace ae::tele::test_tele {
extern std::shared_ptr<ae::tele::IoStreamTrap> trap;
namespace tele_configuration {
struct TeleTrap final : public ITrap {
  struct MetricData {
    std::uint32_t count_{};
    std::uint32_t duration_{};
  };
  class LogLineWriter final : public ILogLine {
   public:
    explicit LogLineWriter(std::vector<std::string>& line) : log_line{line} {}
    void InvokeTime(TimePoint time) override {
      log_line.emplace_back(Format("{:time}", time));
    }
    void WriteLevel(Level level) override {
      log_line.emplace_back(level.Text());
    }
    void WriteModule(Module const& module) override {
      log_line.emplace_back(module.name);
    }
    void Location(std::string_view file, std::uint32_t line) override {
      log_line.emplace_back(Format("{file}:{line}", file, line));
    }
    void TagName(std::string_view name) override {
      log_line.emplace_back(name);
    }
    void Blob(std::span<std::uint8_t const> blob) override {
      log_line.emplace_back(reinterpret_cast<char const*>(blob.data()),
                            blob.size());
    }
    std::vector<std::string>& log_line;
  };
  void AddInvoke(Tag const& tag, std::uint32_t count) override {
    metric_data_[tag.index()].count_ += count;
  }
  void AddInvokeDuration(Tag const& tag, Duration duration) override {
    metric_data_[tag.index()].duration_ +=
        static_cast<std::uint32_t>(duration.count());
  }
  void LogLine(Tag const& tag, ILogCollector& log_collector) override {
    auto& line = log_lines_.emplace_back();
    line.emplace_back(std::to_string(tag.index()));
    auto writer = LogLineWriter{line};
    log_collector.WriteLine(writer);
  }
  void WriteEnvData(EnvData const&) override {}
  std::list<std::vector<std::string>> log_lines_;
  std::map<std::size_t, MetricData> metric_data_;
};

template <::ae::tele::TeleConfig Config = ::ae::tele::TeleConfig{}>
struct ConfigProvider {
  template <Level::underlined_t, std::uint32_t>
  static consteval auto GetTeleConfig() {
    return Config;
  }
  static consteval auto GetEnvConfig() {
    return EnvConfig{.static_info = true, .runtime_info = true};
  }
};
}  // namespace tele_configuration

void AssertTimestampShape(std::string const& timestamp);
void test_Register();
void test_SimpleTeleWithDuration();
void test_TeleConfigurations();
void test_TeleProxyTrap();
void test_MergeStatisticsTrap();
void test_StatisticsRotation();
void test_SaveLoadTeleStatistics();
void test_IoStreamTrapFullOutput();
void test_IoStreamTrapLocationWithoutSeparatorUsesUnknownFile();
void test_EnvTele();

}  // namespace ae::tele::test_tele

#endif  // AE_TELE_TEST_COMMON_H_
