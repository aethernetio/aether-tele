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

#ifndef AETHER_TELE_SPACE_EVENT_H_
#define AETHER_TELE_SPACE_EVENT_H_

#include <cstddef>
#include <cstdint>
#include <utility>

#include "aether-tele/levels.h"
#include "aether-tele/space/schema.h"
#include "aether-tele/space/storage.h"

namespace ae::tele::space {

constexpr std::size_t PayloadArity(PayloadKind kind) {
  switch (kind) {
    case PayloadKind::kNone:
      return 0;
    case PayloadKind::kPackedU32:
    case PayloadKind::kPackedU64:
      return 1;
    case PayloadKind::kPackedU32U32:
      return 2;
  }
  return 0;
}

struct DisabledEvent {
  template <typename... Args>
  constexpr explicit DisabledEvent(Args&&...) noexcept {}
};

template <typename StreamT, Tag kTag, Level::underlined_t LevelV>
class Event {
 public:
  using Config = typename StreamT::ConfigType;
  using TimeType = typename Config::TimeType;
  using CountCodec = typename Config::CountCodec;
  static constexpr auto kCfg =
      Config::template GetTeleConfig<LevelV, kTag.module_id>();
  static constexpr std::size_t kSlot = TagSlot(Config::kTags, kTag.index);

  template <typename... Args>
  explicit Event(StreamT& stream, Args... args) noexcept : stream_(&stream) {
    static_assert(kSlot < Config::kTags.size(), "tag is not in Config::kTags");
    static_assert(PayloadArity(kTag.payload) == sizeof...(Args),
                  "payload arity does not match schema");
    static_assert(TagMatchesType(kTag), "record type does not match flags");

    if constexpr (kCfg.count_metrics && kTag.writes_metric()) {
      stream.AddCount(kSlot);
    }

    TimeType now = TimeType{};
    static constexpr bool kNeedNow =
        (kCfg.logs_enabled && kTag.writes_log() && kTag.timed()) ||
        (kCfg.time_metrics && kTag.has_duration());
    if constexpr (kNeedNow) {
      now = Config::Clock::now();
      start_ = now;
    }

    if constexpr (kCfg.logs_enabled && kTag.writes_log()) {
      stream.BeginEvent(kTag.index, kTag.timed(), now);
      WritePayload(stream, args...);
    }
  }

  ~Event() noexcept {
    if constexpr (kCfg.time_metrics && kTag.has_duration()) {
      auto const end = Config::Clock::now();
      auto const duration = end - start_;
      stream_->AddDuration(kSlot, duration);
      if constexpr (kCfg.logs_enabled && kTag.writes_log()) {
        stream_->WriteDuration(duration);
      }
    }
  }

  Event(Event const&) = delete;
  Event& operator=(Event const&) = delete;

 private:
  template <typename... Args>
  static void WritePayload(StreamT& stream, Args... args) {
    if constexpr (kTag.payload == PayloadKind::kNone) {
      (void)stream;
    } else if constexpr (kTag.payload == PayloadKind::kPackedU32U32) {
      WriteOne(stream, args...);
    } else {
      WriteOne(stream, args...);
    }
  }

  template <typename A0>
  static void WriteOne(StreamT& stream, A0 a0) {
    stream.template WriteValue<CountCodec>(static_cast<std::uint64_t>(a0));
  }

  template <typename A0, typename A1>
  static void WriteOne(StreamT& stream, A0 a0, A1 a1) {
    stream.template WriteValue<CountCodec>(static_cast<std::uint64_t>(a0));
    stream.template WriteValue<CountCodec>(static_cast<std::uint64_t>(a1));
  }

  StreamT* stream_;
  TimeType start_{};
};

template <typename StreamT, Tag kTag, Level::underlined_t LevelV>
constexpr bool StreamEnabled() {
  using Config = typename StreamT::ConfigType;
  return Config::template GetTeleConfig<LevelV, kTag.module_id>().Any();
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_EVENT_H_
