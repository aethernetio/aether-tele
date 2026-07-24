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
#include <cstdint>
#include <span>
#include <sstream>
#include <utility>

namespace ae::tele::test_tele {
template <typename WriteFn>
struct LambdaLogCollector final : public ILogCollector {
  explicit LambdaLogCollector(WriteFn&& wf) : write_fn{std::move(wf)} {}
  void WriteLine(ILogLine& log_line) override { write_fn(log_line); }
  WriteFn write_fn;
};

void test_IoStreamTrapFullOutput() {
  auto stream = std::ostringstream{};
  auto trap = IoStreamTrap{stream};
  auto const fixed_time =
      TimePoint{std::chrono::hours{3} + std::chrono::minutes{4} +
                std::chrono::seconds{5} + std::chrono::microseconds{123456}};
  auto const& tag = ::Test;
  auto log_collector = LambdaLogCollector{[&](ILogLine& log_line) {
    log_line.InvokeTime(fixed_time);
    log_line.WriteLevel(Level{Level::kDebug});
    log_line.WriteModule(::TestObj);
    log_line.Location("src/test-tele.cpp", 42);
    log_line.TagName(tag.name);
    log_line.Blob(
        std::span{reinterpret_cast<std::uint8_t const*>("message 12"), 10});
  }};
  trap.LogLine(tag, log_collector);
  auto const output = stream.str();
  TEST_ASSERT_EQUAL_STRING(
      "  12:[03:04:05.123456]:kDebug:TestObj:test-tele.cpp:42:Test:message "
      "12\n",
      output.c_str());
}

void test_IoStreamTrapLocationWithoutSeparatorUsesUnknownFile() {
  auto stream = std::ostringstream{};
  auto trap = IoStreamTrap{stream};
  auto const& tag = ::Test;
  auto log_collector = LambdaLogCollector{
      [&](ILogLine& log_line) { log_line.Location("test-tele.cpp", 42); }};
  trap.LogLine(tag, log_collector);
  auto const output = stream.str();
  TEST_ASSERT_EQUAL_STRING("  12:UNKNOWN FILE:42\n", output.c_str());
}
}  // namespace ae::tele::test_tele
