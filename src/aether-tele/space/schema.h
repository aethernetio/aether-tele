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

#ifndef AETHER_TELE_SPACE_SCHEMA_H_
#define AETHER_TELE_SPACE_SCHEMA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aether-tele/levels.h"
#include "aether-tele/space/crc.h"

namespace ae::tele::space {

// ATS1 wire version. Layout is documented in space/blob.h.
inline constexpr std::uint8_t kSpaceFormatVersion = 2;
inline constexpr std::uint8_t kMetricsFormatVersion = 1;
inline constexpr std::uint8_t kMagic[4] = {'A', 'T', 'S', '1'};

// uint32 Unix seconds wrap at 2106-02-07 06:28:15 UTC (2^32 - 1).
inline constexpr std::uint32_t kUnixTimeMax = 0xFFFFFFFFu;
inline constexpr std::uint32_t kUnixOverflowYear = 2106;

enum class RecordType : std::uint8_t {
  kLog = 1,
  kDuration = 2,
  kMetric = 3,
};

enum class PayloadKind : std::uint8_t {
  kNone = 0,
  kPackedU32 = 1,
  kPackedU64 = 2,
  kPackedU32U32 = 3,
};

inline constexpr std::uint8_t kFlagTimed = 1 << 0;
inline constexpr std::uint8_t kFlagDuration = 1 << 1;
inline constexpr std::uint8_t kFlagLog = 1 << 2;
inline constexpr std::uint8_t kFlagMetric = 1 << 3;

// Runtime schema row. No names, paths, or level strings.
struct Tag {
  std::uint32_t index{};
  RecordType type{RecordType::kLog};
  PayloadKind payload{PayloadKind::kNone};
  std::uint8_t flags{};
  std::uint8_t severity{};
  std::uint32_t module_id{};

  constexpr bool timed() const { return (flags & kFlagTimed) != 0; }
  constexpr bool has_duration() const { return (flags & kFlagDuration) != 0; }
  constexpr bool writes_log() const { return (flags & kFlagLog) != 0; }
  constexpr bool writes_metric() const { return (flags & kFlagMetric) != 0; }
};

inline constexpr std::uint8_t FlagsFor(RecordType type, bool timed) {
  std::uint8_t flags = kFlagMetric;
  switch (type) {
    case RecordType::kLog:
      flags = static_cast<std::uint8_t>(kFlagLog | kFlagMetric |
                                        (timed ? kFlagTimed : 0));
      break;
    case RecordType::kDuration:
      flags = static_cast<std::uint8_t>(kFlagLog | kFlagMetric | kFlagDuration |
                                        kFlagTimed);
      break;
    case RecordType::kMetric:
      flags = kFlagMetric;
      break;
  }
  return flags;
}

constexpr bool TagMatchesType(Tag const& tag) {
  switch (tag.type) {
    case RecordType::kLog:
      return tag.writes_log() && !tag.has_duration();
    case RecordType::kDuration:
      return tag.writes_log() && tag.has_duration() && tag.timed();
    case RecordType::kMetric:
      return !tag.writes_log() && tag.writes_metric();
  }
  return false;
}

struct SpaceTeleConfig {
  bool count_metrics = true;
  bool time_metrics = true;
  bool logs_enabled = true;

  constexpr bool Any() const {
    return count_metrics || time_metrics || logs_enabled;
  }
};

enum class StreamMarker : std::uint8_t {
  kMain = 0,
  kPoller = 1,
  kNetwork = 2,
  kStorage = 3,
  kWorker0 = 4,
};

constexpr std::string_view MarkerName(StreamMarker marker) {
  switch (marker) {
    case StreamMarker::kMain:
      return "Main";
    case StreamMarker::kPoller:
      return "Poller";
    case StreamMarker::kNetwork:
      return "Network";
    case StreamMarker::kStorage:
      return "Storage";
    case StreamMarker::kWorker0:
      return "Worker0";
    default:
      return "Unknown";
  }
}

template <std::size_t N>
constexpr bool UniqueDirectIndices(std::array<Tag, N> const& tags) {
  for (std::size_t i = 0; i < N; ++i) {
    if (tags[i].index > kUnixTimeMax) {
      return false;
    }
    if (!TagMatchesType(tags[i])) {
      return false;
    }
    for (std::size_t j = i + 1; j < N; ++j) {
      if (tags[i].index == tags[j].index) {
        return false;
      }
    }
  }
  return true;
}

template <std::size_t N>
constexpr Tag const* FindTag(std::array<Tag, N> const& tags,
                             std::uint32_t index) {
  for (auto const& tag : tags) {
    if (tag.index == index) {
      return &tag;
    }
  }
  return nullptr;
}

template <std::size_t N>
constexpr std::size_t TagSlot(std::array<Tag, N> const& tags,
                              std::uint32_t index) {
  for (std::size_t i = 0; i < N; ++i) {
    if (tags[i].index == index) {
      return i;
    }
  }
  return N;
}

template <typename C>
constexpr std::size_t ConfigMaxStreams() {
  if constexpr (requires { C::kMaxStreams; }) {
    return C::kMaxStreams;
  } else {
    return 8;
  }
}

template <typename C>
constexpr std::size_t ConfigStreamCapacity() {
  if constexpr (requires { C::kStreamCapacity; }) {
    return C::kStreamCapacity;
  } else {
    return 4096;
  }
}

template <typename C>
constexpr std::uint32_t ConfigTicksPerSecond() {
  if constexpr (requires { C::kTicksPerSecond; }) {
    return C::kTicksPerSecond;
  } else {
    return 1'000'000u;
  }
}

template <std::size_t NTags>
constexpr std::uint32_t ComputeSchemaChecksum(
    std::uint8_t format_version, std::uint8_t metrics_format,
    std::uint32_t count_sig, std::uint32_t time_sig, std::uint32_t delta_sig,
    std::uint32_t index_sig, std::uint32_t ticks_per_second,
    std::array<Tag, NTags> const& tags) {
  Crc32Acc crc;
  crc.FeedU8(format_version);
  crc.FeedU8(metrics_format);
  crc.FeedU32(count_sig);
  crc.FeedU32(time_sig);
  crc.FeedU32(delta_sig);
  crc.FeedU32(index_sig);
  crc.FeedU32(ticks_per_second);
  crc.FeedU32(static_cast<std::uint32_t>(NTags));
  for (auto const& tag : tags) {
    crc.FeedU32(tag.index);
    crc.FeedU8(static_cast<std::uint8_t>(tag.type));
    crc.FeedU8(static_cast<std::uint8_t>(tag.payload));
    crc.FeedU8(tag.flags);
  }
  return crc.Finish();
}

#if defined(AE_TELE_SPACE_HOST)
inline constexpr std::string_view SeverityName(std::uint8_t level) {
  switch (level) {
    case Level::kInfo:
      return "Info";
    case Level::kWarning:
      return "Warning";
    case Level::kError:
      return "Error";
    case Level::kDebug:
      return "Debug";
    default:
      return "Unknown";
  }
}

struct HostTag {
  std::uint32_t index{};
  std::string_view name{};
  std::string_view module{};
  std::uint8_t severity{};
};

template <std::size_t N>
constexpr HostTag const* FindHostTag(std::array<HostTag, N> const& tags,
                                     std::uint32_t index) {
  for (auto const& tag : tags) {
    if (tag.index == index) {
      return &tag;
    }
  }
  return nullptr;
}
#endif

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_SCHEMA_H_
