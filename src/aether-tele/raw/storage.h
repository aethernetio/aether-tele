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
#ifndef AETHER_TELE_RAW_STORAGE_H_
#define AETHER_TELE_RAW_STORAGE_H_
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include "aether-tele/raw/types.h"
#include "aether-tele/raw/wire.h"

namespace ae::tele::raw {
// A Stream has one owner. Each callback accepts a complete immutable block.
// No destructor IO; drain explicitly while callback/context are alive.
template <class SpaceT, std::size_t Capacity = 4096,
          std::size_t MetricCapacity = 128>
class Stream {
 public:
  using SpaceType = SpaceT;
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kMetricCapacity = MetricCapacity;
  static_assert(Capacity >= kHeaderSize);
  static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max());
  static_assert(Capacity <= std::numeric_limits<std::size_t>::max() -
                                wire::kBlockPrefixSlack);
  struct DurationTicket {
    std::uint64_t generation{};
    std::size_t offset{};
    bool valid{};
    Stream const* owner{};
  };
  explicit Stream(SpaceT& space, std::uint32_t stream_id,
                  DrainCallback drain = nullptr, void* drain_context = nullptr,
                  ConsoleCallback console = nullptr,
                  void* console_context = nullptr) noexcept
      : space_(space),
        stream_id_(stream_id),
        drain_(drain),
        drain_context_(drain_context),
        console_(console),
        console_context_(console_context) {}
  Stream(Stream const&) = delete;
  Stream& operator=(Stream const&) = delete;
  Stream(Stream&&) = delete;
  Stream& operator=(Stream&&) = delete;
  SpaceT& space() noexcept { return space_; }
  SpaceT const& space() const noexcept { return space_; }
  std::uint32_t id() const noexcept { return stream_id_; }

  bool RestoreMetrics(std::span<MetricTotal const> totals) noexcept {
    if (busy_ || restore_closed_ || totals.size() > MetricCapacity)
      return false;
    for (std::size_t i = 0; i < totals.size(); ++i) {
      if (totals[i].index > wire::kMaxTag ||
          (totals[i].metric.samples &&
           totals[i].metric.min > totals[i].metric.max))
        return false;
      for (std::size_t j = 0; j < i; ++j)
        if (totals[j].index == totals[i].index) return false;
    }
    for (std::size_t i = 0; i < totals.size(); ++i) {
      metrics_[i].occupied = true;
      metrics_[i].index = totals[i].index;
      metrics_[i].metric = totals[i].metric;
    }
    restore_closed_ = true;
    return true;
  }

  bool Emit(Tag<SpaceT> const& tag, Kind kind, Options snapshot, Data data = {},
            std::uint64_t duration = 0, std::uint64_t timestamp = 0,
            DurationTicket* ticket = nullptr) noexcept {
    if (ticket) *ticket = {};
    snapshot = Restrict(snapshot, SpaceT::kCompiled);
    bool const to_console = snapshot.console && console_;
    if (!snapshot.logs && !to_console) return true;
    restore_closed_ = true;
    if (busy_ || sequence_exhausted_ || stream_id_ > wire::kMaxStream ||
        tag.index > wire::kMaxTag || kind != Kind::kLog) {
      Increment(dropped_);
      return false;
    }
    BusyGuard guard{busy_};
    auto const fields = snapshot.fields;
    auto const sequence = sequence_;
    // First encode a standalone block, copying ALL incoming payload before IO.
    // The same scratch block is also an independently decodable console event.
    std::size_t cursor = 0;
    std::size_t row{}, time_at{}, duration_at{};
    bool const wide = sequence > wire::SequenceCodec::kUpper;
    if (!Header(scratch_, cursor, wire::kLogBlock, fields, wide, sequence,
                timestamp))
      return Drop();
    row = cursor;
    if (!wire::Write<wire::TagCodec>(tag.index, scratch_, cursor))
      return Drop();
    time_at = cursor;
    if ((fields & kTime) && !wire::Write<wire::TimeCodec>(0, scratch_, cursor))
      return Drop();
    duration_at = cursor;
    if (fields & kDuration) {
      if (cursor == scratch_.size()) return Drop();
      scratch_[cursor++] =
          ticket ? wire::kDurationUnknown : wire::EncodeDuration(duration);
    }
    if (fields & kPayload) {
      if (!wire::Write<wire::BlobCodec>(data.payload.size(), scratch_,
                                        cursor) ||
          data.payload.size() > scratch_.size() - cursor)
        return Drop();
      if (!data.payload.empty())
        std::memcpy(scratch_.data() + cursor, data.payload.data(),
                    data.payload.size());
      cursor += data.payload.size();
    }
    auto const standalone_size = cursor;
    auto const console_bytes = wire::FinishBlock(scratch_, cursor, Capacity);
    if (console_bytes.empty()) return Drop();
    bool success = true, persisted = false;
    if (snapshot.logs) {
      bool append = used_ && !frozen_ && fields == fields_ && !wide_ && !wide &&
                    sequence == next_block_sequence_;
      std::uint16_t dt = 0;
      if (append && (fields & kTime)) {
        append = timestamp >= last_time_ &&
                 timestamp - last_time_ <= wire::kMaxTimeDeltaUs;
        if (append) dt = wire::EncodeTime(timestamp - last_time_);
      }
      std::array<std::uint8_t, 4> dt_bytes{};
      std::size_t dt_size = 0;
      if (fields & kTime) {
        if (!wire::Write<wire::TimeCodec>(dt, dt_bytes, dt_size)) return Drop();
      }
      auto const row_size =
          standalone_size - row + ((fields & kTime) ? dt_size - 1 : 0);
      auto const appended_size = wire::BlockWireSize(
          used_ ? used_ - wire::kBlockPrefixReserve + row_size : 0);
      if (append && (!appended_size || appended_size > Capacity ||
                     row_size > bytes_.size() - used_ ||
                     ((fields & kTime) &&
                      dt_size - 1 > scratch_.size() - standalone_size)))
        append = false;
      if (!append && used_ && !DrainPending()) {
        success = false;
        Increment(dropped_);
      } else {
        if (append) {
          // Variable time code is inserted only in the private scratch arena.
          if ((fields & kTime) && dt_size > 1)
            std::memmove(scratch_.data() + time_at + dt_size,
                         scratch_.data() + time_at + 1,
                         standalone_size - time_at - 1);
          if (fields & kTime)
            std::memcpy(scratch_.data() + time_at, dt_bytes.data(), dt_size);
          auto const offset = used_;
          std::memcpy(bytes_.data() + used_, scratch_.data() + row, row_size);
          used_ += row_size;
          if (ticket && (fields & kDuration) && !slots_disabled_)
            *ticket = {generation_,
                       offset + duration_at - row +
                           ((fields & kTime) ? dt_size - 1 : 0),
                       true, this};
          if (fields & kTime) last_time_ += wire::TimeValue(dt);
          // Restore standalone scratch for the console without reading caller
          // data.
          if ((fields & kTime) && dt_size > 1)
            std::memmove(scratch_.data() + time_at + 1,
                         scratch_.data() + time_at + dt_size,
                         standalone_size - time_at - 1);
          if (fields & kTime) scratch_[time_at] = 0;
        } else {
          std::memcpy(bytes_.data(), scratch_.data(), standalone_size);
          used_ = standalone_size;
          fields_ = fields;
          wide_ = wide;
          frozen_ = false;
          last_time_ = timestamp;
          if (ticket && (fields & kDuration) && !slots_disabled_)
            *ticket = {generation_, duration_at, true, this};
        }
        auto const block = wire::FinishBlock(bytes_, used_, Capacity);
        wire_start_ = static_cast<std::size_t>(block.data() - bytes_.data());
        next_block_sequence_ = sequence == UINT64_MAX ? sequence : sequence + 1;
        persisted = true;
      }
    }
    if (persisted || to_console) AdvanceSequence();
    if (to_console) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
      try {
#endif
        console_(console_context_, console_bytes);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
      } catch (...) {
        Increment(console_failures_);
        return false;
      }
#endif
    }
    return success;
  }

  bool FinishDuration(DurationTicket const& ticket,
                      std::uint64_t microseconds) noexcept {
    if (busy_ || frozen_ || slots_disabled_ || !ticket.valid ||
        ticket.owner != this || ticket.generation != generation_ ||
        ticket.offset >= used_ ||
        bytes_[ticket.offset] != wire::kDurationUnknown)
      return false;
    bytes_[ticket.offset] = wire::EncodeDuration(microseconds);
    return true;
  }
  bool Drain() noexcept {
    restore_closed_ = true;
    if (busy_) return false;
    BusyGuard guard{busy_};
    return DrainPending();
  }
  bool DrainMetrics() noexcept {
    restore_closed_ = true;
    if (busy_) return false;
    BusyGuard guard{busy_};
    if (!DrainPending()) return false;
    for (auto& entry : metrics_) {
      if (!entry.occupied || !HasDelta(entry.delta)) continue;
      if (!drain_ || sequence_exhausted_) return false;
      entry.retry = entry.delta;
      entry.delta = {};
      entry.retry_sequence = sequence_;
      entry.retry_pending = true;
      AdvanceSequence();
      if (!DrainMetricRetry(entry)) return false;
    }
    return true;
  }
  std::span<std::uint8_t const> pending() const noexcept {
    return used_ ? std::span<std::uint8_t const>{bytes_.data() + wire_start_,
                                                 used_ - wire_start_}
                 : std::span<std::uint8_t const>{};
  }
  std::uint64_t dropped() const noexcept { return dropped_; }
  std::uint64_t metric_overflow() const noexcept { return metric_overflow_; }
  std::uint64_t console_failures() const noexcept { return console_failures_; }
  void Count(std::uint32_t index) noexcept {
    restore_closed_ = true;
    if (auto* e = FindOrInsert(index)) {
      Increment(e->metric.count);
      Increment(e->delta.count);
    }
  }
  void AddDuration(std::uint32_t index, std::uint64_t duration) noexcept {
    restore_closed_ = true;
    if (auto* e = FindOrInsert(index)) {
      UpdateDuration(e->metric, duration);
      UpdateDuration(e->delta, duration);
    }
  }
  Metric MetricFor(std::uint32_t index) const noexcept {
    for (auto const& e : metrics_)
      if (e.occupied && e.index == index) return e.metric;
    return {};
  }
  // Contributions not yet acknowledged by the drain target. Restored and
  // previously acknowledged totals are deliberately excluded: a coordinated
  // host query can add these deltas to canonical backend history exactly once.
  Metric PendingMetricFor(std::uint32_t index) const noexcept {
    for (auto const& e : metrics_) {
      if (!e.occupied || e.index != index) continue;
      auto pending = e.delta;
      if (e.retry_pending) AccumulateMetric(pending, e.retry);
      return pending;
    }
    return {};
  }

 private:
  template <typename>
  friend class Scope;
  void BeginScope() noexcept { restore_closed_ = true; }
  struct BusyGuard {
    bool& busy;
    explicit BusyGuard(bool& b) : busy(b) { busy = true; }
    ~BusyGuard() { busy = false; }
  };
  struct Entry {
    std::uint32_t index{};
    bool occupied{};
    Metric metric{}, delta{}, retry{};
    std::uint64_t retry_sequence{};
    bool retry_pending{};
  };
  static bool HasDelta(Metric const& m) noexcept {
    return m.count || m.samples;
  }
  static void Increment(std::uint64_t& n) noexcept {
    if (n != UINT64_MAX) ++n;
  }
  static void UpdateDuration(Metric& m, std::uint64_t d) noexcept {
    if (!m.samples)
      m.min = m.max = d;
    else {
      m.min = std::min(m.min, d);
      m.max = std::max(m.max, d);
    }
    Increment(m.samples);
    m.sum = d > UINT64_MAX - m.sum ? UINT64_MAX : m.sum + d;
  }
  void AdvanceSequence() noexcept {
    if (sequence_ == UINT64_MAX)
      sequence_exhausted_ = true;
    else
      ++sequence_;
  }
  bool Drop() noexcept {
    Increment(dropped_);
    return false;
  }
  Entry* FindOrInsert(std::uint32_t index) noexcept {
    if (busy_ || index > wire::kMaxTag) {
      Increment(metric_overflow_);
      return nullptr;
    }
    Entry* free = nullptr;
    for (auto& e : metrics_) {
      if (e.occupied && e.index == index) return &e;
      if (!e.occupied && !free) free = &e;
    }
    if (free) {
      free->occupied = true;
      free->index = index;
      return free;
    }
    Increment(metric_overflow_);
    return nullptr;
  }
  bool Header(std::span<std::uint8_t> out, std::size_t& cursor,
              std::uint8_t kind, std::uint32_t fields, bool wide,
              std::uint64_t sequence, std::uint64_t base) noexcept {
    if (out.size() < wire::kBlockPrefixReserve + 1 ||
        stream_id_ > wire::kMaxStream)
      return false;
    cursor = wire::kBlockPrefixReserve;
    out[cursor++] = wire::Control(kind, fields, wide);
    if (!wire::Write<wire::StreamCodec>(stream_id_, out, cursor)) return false;
    if (wide) {
      if (!wire::WriteU64(sequence, out, cursor)) return false;
    } else if (!wire::Write<wire::SequenceCodec>(sequence, out, cursor))
      return false;
    return !(fields & kTime) || wire::WriteU64(base, out, cursor);
  }
  bool DrainMetricRetry(Entry& e) noexcept {
    if (!drain_) return false;
    auto const& m = e.retry;
    bool wide = e.retry_sequence > wire::SequenceCodec::kUpper ||
                m.count > wire::CountCodec::kUpper ||
                m.samples > wire::CountCodec::kUpper ||
                m.sum > wire::SumCodec::kUpper ||
                m.min > wire::SumCodec::kUpper ||
                m.max > wire::SumCodec::kUpper;
    constexpr auto kMetricBlockCapacity =
        wire::kBlockPrefixReserve + 1 + wire::StreamCodec::kMaxWireBytes +
        sizeof(std::uint64_t) + wire::TagCodec::kMaxWireBytes +
        5 * sizeof(std::uint64_t);
    std::array<std::uint8_t, kMetricBlockCapacity> frame{};
    std::size_t cursor = 0;
    if (!Header(frame, cursor, wire::kMetricBlock, 0, wide, e.retry_sequence,
                0) ||
        !wire::Write<wire::TagCodec>(e.index, frame, cursor))
      return false;
    if (wide) {
      for (auto n : {m.count, m.samples, m.sum, m.min, m.max})
        if (!wire::WriteU64(n, frame, cursor)) return false;
    } else if (!wire::Write<wire::CountCodec>(m.count, frame, cursor) ||
               !wire::Write<wire::CountCodec>(m.samples, frame, cursor) ||
               !wire::Write<wire::SumCodec>(m.sum, frame, cursor) ||
               !wire::Write<wire::SumCodec>(m.min, frame, cursor) ||
               !wire::Write<wire::SumCodec>(m.max, frame, cursor))
      return false;
    auto const block = wire::FinishBlock(frame, cursor);
    if (block.empty()) return false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!drain_(drain_context_, block)) return false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      return false;
    }
#endif
    e.retry_pending = false;
    e.retry = {};
    return true;
  }
  bool DrainPending() noexcept {
    // Freeze before invoking any user callback, even an outstanding metric
    // retry.
    if (used_ && !frozen_) {
      frozen_ = true;
      if (generation_ == UINT64_MAX)
        slots_disabled_ = true;
      else
        ++generation_;
    }
    for (auto& e : metrics_)
      if (e.retry_pending && !DrainMetricRetry(e)) return false;
    if (!used_) return true;
    if (!drain_) return false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!drain_(drain_context_, pending())) return false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      return false;
    }
#endif
    used_ = 0;
    frozen_ = false;
    return true;
  }
  SpaceT& space_;
  std::uint32_t const stream_id_;
  DrainCallback drain_;
  void* drain_context_;
  ConsoleCallback console_;
  void* console_context_;
  std::array<std::uint8_t, Capacity + wire::kBlockPrefixSlack> bytes_{},
      scratch_{};
  std::array<Entry, MetricCapacity> metrics_{};
  std::size_t used_{}, wire_start_{};
  std::uint64_t sequence_{}, dropped_{}, metric_overflow_{},
      console_failures_{};
  std::uint64_t generation_{1}, last_time_{}, next_block_sequence_{};
  std::uint32_t fields_{};
  bool busy_{}, sequence_exhausted_{}, restore_closed_{}, frozen_{}, wide_{},
      slots_disabled_{};
};
}  // namespace ae::tele::raw
#endif
