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

#ifndef DEMO_NETWORK_SPACE_H_
#define DEMO_NETWORK_SPACE_H_

#include <array>
#include <cstdint>
#include <string_view>

#include "ae-numeric/exponential.h"
#include "ae-numeric/fixed_point.h"
#include "ae-numeric/numeric_traits.h"
#include "aether-tele/packed_int.h"
#include "aether-tele/space.h"

namespace demo::network {

using Packed = ae::tele::PackedU64;
using Packed32 = ae::tele::PackedU32;

// Exponential delta over the existing ae::Exponential type.
// Runtime (metrics/clock): uint64 microseconds in TeleStorage.
// Exponential runtime: FixedPoint ticks (not a second packed-int codec).
// Wire: PackedU64 exponential codes.
// Min magnitude 1 tick, boundary 4e9 ticks (~4000 s at 1 us/tick).
// BoundaryCode is PackedU64::kMaxBoundaryCode (4-byte packed max).
// Zero is code 0. Values above the boundary saturate.
inline constexpr double kDeltaMinMagnitude = 1.0;
inline constexpr double kDeltaBoundaryMagnitude = 4'000'000'000.0;
inline constexpr auto kDeltaBoundaryCode = Packed::kMaxBoundaryCode;

using DeltaRuntime =
    ae::FixedPoint<std::uint32_t, kDeltaBoundaryMagnitude>;
using DeltaPolicy = ae::tele::space::WideExponentialMathPolicy<
    DeltaRuntime, kDeltaMinMagnitude, kDeltaBoundaryMagnitude,
    kDeltaBoundaryCode, typename ae::numeric_traits<Packed>::rep_value_type,
    false>;
using DeltaExp =
    ae::Exponential<DeltaRuntime, Packed, kDeltaMinMagnitude,
                    kDeltaBoundaryMagnitude, kDeltaBoundaryCode, DeltaPolicy>;

inline constexpr std::uint32_t kModNet = 1;
inline constexpr std::uint32_t kModLink = 2;
inline constexpr std::uint32_t kModOff = 9;

// TELE_INDEX_TABLE_BEGIN
#define DEMO_NET_TAGS(X)                                                     \
  X(Poll, 2000, Log, None, true, kModNet, Info)                              \
  X(PktRx, 1600, Log, PackedU32, true, kModNet, Info)                        \
  X(PktTx, 1500, Log, PackedU32, true, kModNet, Info)                        \
  X(Request, 900, Duration, None, true, kModNet, Info)                       \
  X(Reconnect, 400, Log, None, true, kModLink, Warning)                      \
  X(NetError, 1, Log, PackedU32, true, kModLink, Error)                      \
  X(ByteCount, 80, Metric, None, false, kModNet, Info)                       \
  X(DisabledTap, 50, Log, None, true, kModOff, Debug)
// TELE_INDEX_TABLE_END

DEMO_NET_TAGS(AE_SPACE_POINT)

#if defined(AE_TELE_SPACE_HOST)
constexpr std::string_view NetworkModuleName(std::uint32_t id) {
  switch (id) {
    case kModNet:
      return "Net";
    case kModLink:
      return "Link";
    case kModOff:
      return "Off";
    default:
      return "?";
  }
}
#endif

struct Config {
  using CountType = std::uint32_t;
  using TimeType = std::uint64_t;
  using CountCodec = ae::tele::space::PackedIntCodec<Packed>;
  using TimeCodec = ae::tele::space::PackedIntCodec<Packed>;
  using DeltaCodec = ae::tele::space::ExponentialCodec<DeltaExp>;
  using IndexCodec = ae::tele::space::PackedIntCodec<Packed>;
  using Clock = ae::tele::space::ManualClock<TimeType, Config>;
  using UnixClock = ae::tele::space::ManualUnixClock<Config>;
  static constexpr std::size_t kMaxStreams = 8;
  static constexpr std::size_t kStreamCapacity = 65536;
  static constexpr std::uint32_t kTicksPerSecond = 1'000'000u;

  static constexpr auto kTags = std::array{DEMO_NET_TAGS(AE_SPACE_POINT_REF)};
  static_assert(ae::tele::space::UniqueDirectIndices(kTags));

  static constexpr std::uint32_t kSchemaChecksum =
      ae::tele::space::ComputeSchemaChecksum(
          ae::tele::space::kSpaceFormatVersion,
          ae::tele::space::kMetricsFormatVersion, CountCodec::kSchemaSignature,
          TimeCodec::kSchemaSignature, DeltaCodec::kSchemaSignature,
          IndexCodec::kSchemaSignature, kTicksPerSecond, kTags);

#if defined(AE_TELE_SPACE_HOST)
#define DEMO_NET_HOST(NAME, INDEX, TYPE, PAYLOAD, TIMED, MODULE_ID, SEVERITY) \
  ::ae::tele::space::HostTag{static_cast<std::uint32_t>(INDEX), #NAME,        \
                             NetworkModuleName(MODULE_ID),                    \
                             ::ae::tele::Level::k##SEVERITY},
  static constexpr auto kHostTags = std::array{DEMO_NET_TAGS(DEMO_NET_HOST)};
#undef DEMO_NET_HOST
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

}  // namespace demo::network

#define NET_TELE_DEBUG(STREAM, TAG, ...) \
  AE_SPACE_TELE_DEBUG(STREAM, TAG, __VA_ARGS__)
#define NET_TELE_INFO(STREAM, TAG, ...) \
  AE_SPACE_TELE_INFO(STREAM, TAG, __VA_ARGS__)
#define NET_TELE_WARNING(STREAM, TAG, ...) \
  AE_SPACE_TELE_WARNING(STREAM, TAG, __VA_ARGS__)
#define NET_TELE_ERROR(STREAM, TAG, ...) \
  AE_SPACE_TELE_ERROR(STREAM, TAG, __VA_ARGS__)

#endif  // DEMO_NETWORK_SPACE_H_
