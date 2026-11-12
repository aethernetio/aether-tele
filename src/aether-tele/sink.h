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

#ifndef AETHER_TELE_SINK_H_
#define AETHER_TELE_SINK_H_

#include <atomic>
#include <concepts>
#include <memory>
#include <utility>

#include "aether-tele/itrap.h"
#include "aether-tele/levels.h"
#include "aether-tele/modules.h"  // IWYU pragma: keep

namespace ae::tele {
namespace sink_internal {
// Keep the C++11 shared_ptr atomics for standard libraries without the C++20
// specialization. Suppress only these deprecated compatibility calls; consumers
// may still enable -Werror and deprecation diagnostics for their own code.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4996)
#endif
template <typename T>
std::shared_ptr<T> LoadShared(std::shared_ptr<T> const& slot) {
  return std::atomic_load_explicit(&slot, std::memory_order_acquire);
}
template <typename T>
void StoreShared(std::shared_ptr<T>& slot, std::shared_ptr<T> value) {
  std::atomic_store_explicit(&slot, std::move(value),
                             std::memory_order_release);
}
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
}  // namespace sink_internal
struct TeleConfig {
  bool count_metrics = true;
  bool time_metrics = true;

  bool logs_enabled = true;
  bool start_time_logs = true;
  bool level_module_logs = true;
  bool location_logs = true;
  bool name_logs = true;
  bool blob_logs = true;
};

struct EnvConfig {
  bool static_info = true;
  bool runtime_info = true;
};

template <typename T>
concept ConfigProvider = requires() {
  {
    T::template GetTeleConfig<ae::tele::Level::kDebug, 12>()
  } -> std::same_as<TeleConfig>;
  { T::GetEnvConfig() } -> std::same_as<EnvConfig>;
};

template <ConfigProvider CP>
class TeleSink {
 public:
  using ConfigProviderType = CP;

  template <Level::underlined_t l, std::uint32_t m>
  static consteval auto GetTeleConfig() {
    return ConfigProviderType::template GetTeleConfig<l, m>();
  }
  static consteval auto GetEnvConfig() {
    return ConfigProviderType::GetEnvConfig();
  }

  static TeleSink& Instance() {
    static TeleSink sink;
    return sink;
  }

  void SetTrap(std::shared_ptr<ITrap> const& trap) {
    // Take a copy before publishing; an existing Tele keeps its original trap
    // alive until its RAII duration is recorded.
    sink_internal::StoreShared(trap_, trap);
  }

  std::shared_ptr<ITrap> GetTrap() const {
    return sink_internal::LoadShared(trap_);
  }

  // Legacy reference getter retained for source compatibility. It is only safe
  // while trap replacement is quiescent; concurrent readers must use GetTrap().
  constexpr auto const& trap() const { return trap_; }

 private:
  std::shared_ptr<ITrap> trap_;
};

// Explicit namespace identity for consumers sharing the same configuration.
// The existing TeleSink<CP> and its instance remain unchanged.
template <typename Identity, ConfigProvider CP>
class NamespacedTeleSink : public TeleSink<CP> {
 public:
  static NamespacedTeleSink& Instance() {
    static NamespacedTeleSink sink;
    return sink;
  }
};

namespace sink_internal {
// Existing custom sinks need only implement trap(); new TeleSink users obtain
// a synchronized owning snapshot without changing the historical getter API.
template <typename Sink>
std::shared_ptr<ITrap> TrapSnapshot(Sink& sink) {
  if constexpr (requires {
                  {
                    sink.GetTrap()
                  } -> std::convertible_to<std::shared_ptr<ITrap>>;
                }) {
    return sink.GetTrap();
  } else {
    return sink.trap();
  }
}
}  // namespace sink_internal
}  // namespace ae::tele

#endif  // AETHER_TELE_SINK_H_
