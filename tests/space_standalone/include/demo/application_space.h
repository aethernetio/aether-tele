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

#ifndef DEMO_APPLICATION_SPACE_H_
#define DEMO_APPLICATION_SPACE_H_

#include <array>
#include <cstdint>
#include <string_view>

#include "aether-tele/packed_int.h"
#include "aether-tele/space.h"

namespace demo::application {

using Packed = ae::tele::PackedU64;

inline constexpr std::uint32_t kModUi = 1;
inline constexpr std::uint32_t kModCore = 2;
inline constexpr std::uint32_t kModOff = 9;

// Packed fixed-point delta: runtime is uint64 ticks; wire is PackedU64
// (ae-numeric TieredInt). Exact round-trip. Tiers 1/2/4/8 bytes.
// TELE_INDEX_TABLE_BEGIN
#define DEMO_APP_TAGS(X)                                                     \
  X(Frame, 2000, Log, None, true, kModUi, Info)                              \
  X(Job, 700, Log, PackedU32, true, kModUi, Info)                            \
  X(Work, 900, Duration, None, true, kModUi, Info)                           \
  X(UserEvent, 40, Log, None, true, kModCore, Info)                          \
  X(Fatal, 1, Log, PackedU32, true, kModCore, Error)                         \
  X(JobsDone, 30, Metric, None, false, kModUi, Info)                         \
  X(DisabledAudit, 8, Log, None, true, kModOff, Debug)
// TELE_INDEX_TABLE_END

DEMO_APP_TAGS(AE_SPACE_POINT)

#if defined(AE_TELE_SPACE_HOST)
constexpr std::string_view ApplicationModuleName(std::uint32_t id) {
  switch (id) {
    case kModUi:
      return "Ui";
    case kModCore:
      return "Core";
    case kModOff:
      return "Off";
    default:
      return "?";
  }
}
#endif

struct Config {
  using CountType = std::uint64_t;
  using TimeType = std::uint64_t;
  using CountCodec = ae::tele::space::PackedIntCodec<Packed>;
  using TimeCodec = ae::tele::space::PackedIntCodec<Packed>;
  using DeltaCodec = ae::tele::space::PackedIntCodec<Packed>;
  using IndexCodec = ae::tele::space::PackedIntCodec<Packed>;
  using Clock = ae::tele::space::ManualClock<TimeType, Config>;
  using UnixClock = ae::tele::space::ManualUnixClock<Config>;
  static constexpr std::size_t kMaxStreams = 8;
  static constexpr std::size_t kStreamCapacity = 65536;
  static constexpr std::uint32_t kTicksPerSecond = 1'000u;

  static constexpr auto kTags = std::array{DEMO_APP_TAGS(AE_SPACE_POINT_REF)};
  static_assert(ae::tele::space::UniqueDirectIndices(kTags));

  static constexpr std::uint32_t kSchemaChecksum =
      ae::tele::space::ComputeSchemaChecksum(
          ae::tele::space::kSpaceFormatVersion,
          ae::tele::space::kMetricsFormatVersion, CountCodec::kSchemaSignature,
          TimeCodec::kSchemaSignature, DeltaCodec::kSchemaSignature,
          IndexCodec::kSchemaSignature, kTicksPerSecond, kTags);

#if defined(AE_TELE_SPACE_HOST)
#define DEMO_APP_HOST(NAME, INDEX, TYPE, PAYLOAD, TIMED, MODULE_ID, SEVERITY) \
  ::ae::tele::space::HostTag{static_cast<std::uint32_t>(INDEX), #NAME,        \
                             ApplicationModuleName(MODULE_ID),                \
                             ::ae::tele::Level::k##SEVERITY},
  static constexpr auto kHostTags = std::array{DEMO_APP_TAGS(DEMO_APP_HOST)};
#undef DEMO_APP_HOST
#endif

  template <ae::tele::Level::underlined_t, std::uint32_t ModuleId>
  static consteval ae::tele::space::SpaceTeleConfig GetTeleConfig() {
    if constexpr (ModuleId == kModOff) {
      return ae::tele::space::SpaceTeleConfig{false, false, false};
    }
    return ae::tele::space::SpaceTeleConfig{};
  }
};

using Storage = ae::tele::space::TeleStorage<Config>;
using Stream = ae::tele::space::Stream<Config>;
using Clock = Config::Clock;
using UnixClock = Config::UnixClock;

}  // namespace demo::application

#define APP_TELE_DEBUG(STREAM, TAG, ...) \
  AE_SPACE_TELE_DEBUG(STREAM, TAG, __VA_ARGS__)
#define APP_TELE_INFO(STREAM, TAG, ...) \
  AE_SPACE_TELE_INFO(STREAM, TAG, __VA_ARGS__)
#define APP_TELE_WARNING(STREAM, TAG, ...) \
  AE_SPACE_TELE_WARNING(STREAM, TAG, __VA_ARGS__)
#define APP_TELE_ERROR(STREAM, TAG, ...) \
  AE_SPACE_TELE_ERROR(STREAM, TAG, __VA_ARGS__)

#endif  // DEMO_APPLICATION_SPACE_H_
