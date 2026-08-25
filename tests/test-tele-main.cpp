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

int main() {
  UNITY_BEGIN();
  RUN_TEST(ae::tele::test_tele::test_Register);
  RUN_TEST(ae::tele::test_tele::test_SimpleTeleWithDuration);
  RUN_TEST(ae::tele::test_tele::test_TeleConfigurations);
  RUN_TEST(ae::tele::test_tele::test_TeleProxyTrap);
  RUN_TEST(ae::tele::test_tele::test_MergeStatisticsTrap);
  RUN_TEST(ae::tele::test_tele::test_StatisticsRotation);
  RUN_TEST(ae::tele::test_tele::test_SaveLoadTeleStatistics);
  RUN_TEST(ae::tele::test_tele::test_IoStreamTrapFullOutput);
  RUN_TEST(ae::tele::test_tele::
               test_IoStreamTrapLocationWithoutSeparatorUsesUnknownFile);
  RUN_TEST(ae::tele::test_tele::test_EnvTele);
  RUN_TEST(ae::tele::test_tele::test_PackedU64WireGolden);
  RUN_TEST(ae::tele::test_tele::test_PackedU32WireGolden);
  return UNITY_END();
}
