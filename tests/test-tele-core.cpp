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

namespace ae::tele::test_tele {
void test_Register() {
  TEST_ASSERT_EQUAL(1, ::One.offset);
  TEST_ASSERT_EQUAL(2, ::Two.offset);
  TEST_ASSERT_EQUAL(3, ::Three.offset);
  TEST_ASSERT_EQUAL(4, ::Four.offset);
}

void test_SimpleTeleWithDuration() {
  {
    AE_TELE_DEBUG(::Test1);
    AE_TELE_DEBUG(::Test1, "format {}", 12);
    AE_TELE_INFO(::Test2, "format {}", 24);
    AE_TELE_INFO(::Test3, "format {}", 48);
  }
  auto const& metrics =
      trap->metrics().at(::Test1.offset + ::TestObj.index_start);
  TEST_ASSERT_EQUAL(2, metrics.invocations_count);
}
}  // namespace ae::tele::test_tele
