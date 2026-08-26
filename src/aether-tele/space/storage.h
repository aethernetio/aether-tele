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

#ifndef AETHER_TELE_SPACE_STORAGE_H_
#define AETHER_TELE_SPACE_STORAGE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include "aether-tele/space/codec.h"
#include "aether-tele/space/schema.h"

namespace ae::tele::space {

template <typename Config>
struct TeleMetric {
  using CountType = typename Config::CountType;
  using TimeType = typename Config::TimeType;

  CountType count{};
  TimeType sum{};
  TimeType min{};
  TimeType max{};
  bool has_duration{false};
};

template <typename Config>
class TeleStorage;

template <typename Config>
class Stream {
 public:
  using ConfigType = Config;
  using TimeType = typename Config::TimeType;
  using Storage = TeleStorage<Config>;

  Stream() = default;
  Stream(Storage& storage, StreamMarker marker) noexcept
      : storage_(&storage), slot_(static_cast<std::size_t>(marker)) {
    storage_->Bind(marker);
  }

  Storage* storage() const { return storage_; }
  StreamMarker marker() const {
    return static_cast<StreamMarker>(static_cast<std::uint8_t>(slot_));
  }
  std::size_t slot() const { return slot_; }

  bool Write(std::uint8_t const* data, std::size_t n) noexcept {
    return storage_ != nullptr && storage_->Write(slot_, data, n);
  }

  template <typename Codec>
  bool WriteValue(std::uint64_t value) noexcept {
    std::array<std::uint8_t, Codec::kMaxEncodedSize> buf{};
    auto const n = Codec::Encode(value, buf.data());
    return Write(buf.data(), n);
  }

  bool BeginEvent(std::uint32_t index, bool timed, TimeType now) noexcept;
  bool WriteDuration(TimeType duration) noexcept;

  void AddCount(std::size_t tag_slot) noexcept {
    if (storage_ != nullptr) {
      storage_->AddCount(slot_, tag_slot);
    }
  }
  void AddDuration(std::size_t tag_slot, TimeType duration) noexcept {
    if (storage_ != nullptr) {
      storage_->AddDuration(slot_, tag_slot, duration);
    }
  }

 private:
  Storage* storage_{};
  std::size_t slot_{};
};

template <typename Config>
class TeleStorage {
 public:
  using CountType = typename Config::CountType;
  using TimeType = typename Config::TimeType;
  using Metric = TeleMetric<Config>;
  using IndexCodec = typename Config::IndexCodec;
  using CountCodec = typename Config::CountCodec;
  using DeltaCodec = typename Config::DeltaCodec;
  using TimeCodec = typename Config::TimeCodec;

  static constexpr std::size_t kMaxStreams = ConfigMaxStreams<Config>();
  static constexpr std::size_t kCapacity = ConfigStreamCapacity<Config>();
  static constexpr std::size_t kTagCount = Config::kTags.size();

  TeleStorage()
      : buf_mem_(std::make_unique<std::uint8_t[]>(kMaxStreams * kCapacity)) {}

  void Clear() {
    used_.fill(0);
    bound_.fill(false);
    last_.fill(TimeType{});
    has_last_.fill(false);
    for (auto& row : metrics_) {
      row.fill(Metric{});
    }
    origin_ = {};
    initialized_ = false;
    overflow_ = false;
    dropped_ = 0;
  }

  void Swap(TeleStorage& other) noexcept { using std::swap; swap(*this, other); }

  void SetOrigin(TimeType origin) {
    origin_ = origin;
    initialized_ = true;
    has_last_.fill(false);
  }

  void SetSequence(std::uint32_t sequence) { sequence_ = sequence; }
  void SetBaseUnix(std::uint32_t unix_seconds) { base_unix_ = unix_seconds; }

  bool initialized() const { return initialized_; }
  TimeType origin() const { return origin_; }
  std::uint32_t sequence() const { return sequence_; }
  std::uint32_t base_unix() const { return base_unix_; }
  bool overflow() const { return overflow_; }
  std::uint32_t dropped() const { return dropped_; }

  void Bind(StreamMarker marker) {
    auto const slot = static_cast<std::size_t>(marker);
    if (slot >= kMaxStreams) {
      overflow_ = true;
      return;
    }
    bound_[slot] = true;
    if (!initialized_) {
      origin_ = Config::Clock::now();
      initialized_ = true;
    }
  }

  bool Write(std::size_t slot, std::uint8_t const* data, std::size_t n) {
    if (slot >= kMaxStreams) {
      overflow_ = true;
      ++dropped_;
      return false;
    }
    if (used_[slot] + n > kCapacity) {
      overflow_ = true;
      ++dropped_;
      return false;
    }
    std::memcpy(SlotBuf(slot) + used_[slot], data, n);
    used_[slot] += n;
    return true;
  }

  bool BeginEvent(std::size_t slot, std::uint32_t index, bool timed,
                  TimeType now) {
    if (!initialized_) {
      origin_ = now;
      initialized_ = true;
    }
    if (!WriteValue<IndexCodec>(slot, index)) {
      return false;
    }
    if (!timed) {
      return true;
    }
    TimeType delta = TimeType{};
    if (has_last_[slot]) {
      delta = now - last_[slot];
    } else {
      delta = now - origin_;
    }
    last_[slot] = now;
    has_last_[slot] = true;
    return WriteValue<DeltaCodec>(slot, static_cast<std::uint64_t>(delta));
  }

  bool WriteDuration(std::size_t slot, TimeType duration) {
    return WriteValue<TimeCodec>(slot, static_cast<std::uint64_t>(duration));
  }

  void AddCount(std::size_t slot, std::size_t tag_slot) {
    if (slot >= kMaxStreams || tag_slot >= kTagCount) {
      return;
    }
    metrics_[slot][tag_slot].count += CountType{1};
  }

  void AddDuration(std::size_t slot, std::size_t tag_slot, TimeType duration) {
    if (slot >= kMaxStreams || tag_slot >= kTagCount) {
      return;
    }
    auto& metric = metrics_[slot][tag_slot];
    metric.sum += duration;
    if (!metric.has_duration) {
      metric.min = duration;
      metric.max = duration;
      metric.has_duration = true;
    } else {
      metric.min = std::min(metric.min, duration);
      metric.max = std::max(metric.max, duration);
    }
  }

  std::span<std::uint8_t const> StreamBytes(StreamMarker marker) const {
    auto const slot = static_cast<std::size_t>(marker);
    if (slot >= kMaxStreams) {
      return {};
    }
    return {SlotBuf(slot), used_[slot]};
  }

  bool stream_bound(StreamMarker marker) const {
    auto const slot = static_cast<std::size_t>(marker);
    return slot < kMaxStreams && bound_[slot];
  }

  std::size_t StreamCount() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMaxStreams; ++i) {
      if (bound_[i]) {
        ++n;
      }
    }
    return n;
  }

  Metric MergedMetric(std::size_t tag_slot) const {
    Metric out{};
    if (tag_slot >= kTagCount) {
      return out;
    }
    for (std::size_t s = 0; s < kMaxStreams; ++s) {
      auto const& m = metrics_[s][tag_slot];
      out.count += m.count;
      out.sum += m.sum;
      if (m.has_duration) {
        if (!out.has_duration) {
          out.min = m.min;
          out.max = m.max;
          out.has_duration = true;
        } else {
          out.min = std::min(out.min, m.min);
          out.max = std::max(out.max, m.max);
        }
      }
    }
    return out;
  }

  bool HasAnyMetric() const {
    for (std::size_t t = 0; t < kTagCount; ++t) {
      auto const m = MergedMetric(t);
      if (m.count != CountType{} || m.has_duration) {
        return true;
      }
    }
    return false;
  }

  std::span<std::uint8_t const> log_bytes() const {
    return StreamBytes(StreamMarker::kMain);
  }

  Metric const* TryMetric(std::uint32_t index) const {
    auto const slot = TagSlot(Config::kTags, index);
    if (slot >= kTagCount) {
      return nullptr;
    }
    scratch_ = MergedMetric(slot);
    if (scratch_.count == CountType{} && !scratch_.has_duration) {
      return nullptr;
    }
    return &scratch_;
  }

  std::size_t TotalLogBytes() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMaxStreams; ++i) {
      n += used_[i];
    }
    return n;
  }

 private:
  template <typename Codec>
  bool WriteValue(std::size_t slot, std::uint64_t value) {
    std::array<std::uint8_t, Codec::kMaxEncodedSize> buf{};
    auto const n = Codec::Encode(value, buf.data());
    return Write(slot, buf.data(), n);
  }

  std::uint8_t* SlotBuf(std::size_t slot) {
    return buf_mem_.get() + slot * kCapacity;
  }
  std::uint8_t const* SlotBuf(std::size_t slot) const {
    return buf_mem_.get() + slot * kCapacity;
  }

  std::unique_ptr<std::uint8_t[]> buf_mem_;
  std::array<std::size_t, kMaxStreams> used_{};
  std::array<bool, kMaxStreams> bound_{};
  std::array<TimeType, kMaxStreams> last_{};
  std::array<bool, kMaxStreams> has_last_{};
  std::array<std::array<Metric, kTagCount>, kMaxStreams> metrics_{};
  TimeType origin_{};
  std::uint32_t sequence_{};
  std::uint32_t base_unix_{};
  bool initialized_{false};
  bool overflow_{false};
  std::uint32_t dropped_{};
  mutable Metric scratch_{};
};

template <typename Config>
bool Stream<Config>::BeginEvent(std::uint32_t index, bool timed,
                                TimeType now) noexcept {
  return storage_ != nullptr && storage_->BeginEvent(slot_, index, timed, now);
}

template <typename Config>
bool Stream<Config>::WriteDuration(TimeType duration) noexcept {
  return storage_ != nullptr && storage_->WriteDuration(slot_, duration);
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_STORAGE_H_
