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

#include <cctype>
#include <cstddef>
#include <iostream>
#include <memory>

namespace ae::tele::test_tele {
std::shared_ptr<ae::tele::IoStreamTrap> trap;

void AssertTimestampShape(std::string const& timestamp) {
  TEST_ASSERT_EQUAL(15U, timestamp.size());
  TEST_ASSERT_EQUAL(':', timestamp[2]);
  TEST_ASSERT_EQUAL(':', timestamp[5]);
  TEST_ASSERT_EQUAL('.', timestamp[8]);
  for (auto i = std::size_t{}; i < timestamp.size(); ++i) {
    if ((i == 2) || (i == 5) || (i == 8)) continue;
    TEST_ASSERT(std::isdigit(static_cast<unsigned char>(timestamp[i])) != 0);
  }
}

}  // namespace ae::tele::test_tele

void setUp() {
  ae::tele::test_tele::trap =
      std::make_shared<ae::tele::IoStreamTrap>(std::cout);
  SinkType::Instance().SetTrap(ae::tele::test_tele::trap);
}

void tearDown() {}
