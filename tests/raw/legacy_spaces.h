#ifndef TEST_RAW_LEGACY_SPACES_H_
#define TEST_RAW_LEGACY_SPACES_H_
#include "aether-tele/tele.h"
namespace routing {
struct A {};
struct B {};
using SinkA = ae::tele::NamespacedTeleSink<A, ae::tele::AllEnabledConfig>;
using SinkB = ae::tele::NamespacedTeleSink<B, ae::tele::AllEnabledConfig>;
inline constexpr ae::tele::Module module{1, 10, 20, "module"};
inline constexpr ae::tele::Tag tag{0, module, "checkpoint"};
struct Disabled {
  template <auto, auto> static consteval ae::tele::TeleConfig GetTeleConfig() {
    ae::tele::TeleConfig c{}; c.logs_enabled = false; c.count_metrics = false; c.time_metrics = false; return c;
  }
  static consteval ae::tele::EnvConfig GetEnvConfig() { return {false, false}; }
};
using OffSink = ae::tele::TeleSink<Disabled>;
struct NoBlob {
  template <auto, auto> static consteval ae::tele::TeleConfig GetTeleConfig() {
    ae::tele::TeleConfig c{}; c.blob_logs = false; return c;
  }
  static consteval ae::tele::EnvConfig GetEnvConfig() { return {}; }
};
using NoBlobSink = ae::tele::TeleSink<NoBlob>;
void OrderAB();
void OrderBA();
}
#endif
