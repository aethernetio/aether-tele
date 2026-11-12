/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
// Compiles the real Aether consumer headers rather than a copied configuration.
#include "aether/tele.h"
#include "aether/transport/transport_tele.h"
#include "aether/ae_actions/ae_actions_tele.h"
#include <unity.h>
#include <memory>

namespace {
void test_real_aether_headers_and_statistics_alias() {
  using Sink = ae::tele::TeleSink<ae::AetherTeleConfig>;
  auto trap = std::make_shared<ae::TeleStatisticsTrap>();
  Sink::Instance().SetTrap(trap);
  {
    AE_TELE_INFO(AetherStarted, "consumer {}", 42);
    AE_TELE_DEBUG(kTcpTransportConnect, "connect {}", "127.0.0.1");
    AE_TELED_WARNING("old simple logging {}", 1);
    AE_TELE_ENV();
  }
  TEST_ASSERT_TRUE(Sink::Instance().GetTrap() == trap);
  TEST_ASSERT_TRUE(Sink::Instance().trap() == trap);  // Quiescent legacy getter.
  TEST_ASSERT_TRUE(!trap->metrics_store().metrics.empty());
  TEST_ASSERT_TRUE(trap->log_storage().size() != 0);
  Sink::Instance().SetTrap(nullptr);
}
}
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_real_aether_headers_and_statistics_alias);
  return UNITY_END();
}
