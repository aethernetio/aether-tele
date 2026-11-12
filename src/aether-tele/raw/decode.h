/*
 * Copyright 2026 Aethernet Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef AETHER_TELE_RAW_DECODE_H_
#define AETHER_TELE_RAW_DECODE_H_

// Host-side reader. Views in DecodedRecord borrow the input byte storage.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "aether-tele/raw/types.h"
#include "aether-tele/raw/wire.h"

namespace ae::tele::raw {

struct DecodedRecord {
  std::uint32_t index{};
  std::uint32_t stream{};
  std::uint32_t fields{};
  std::uint64_t sequence{};
  Kind kind{};
  std::optional<std::uint64_t> time;
  std::optional<std::uint64_t> duration;
  DurationState duration_state{DurationState::kUnknown};
  std::uint8_t duration_code{wire::kDurationUnknown};
  std::span<std::uint8_t const> payload;
  std::optional<Metric> metric;
};

enum class DecodeStatus { kOk, kTruncated, kMalformed };
struct DecodeResult {
  std::vector<DecodedRecord> records;
  DecodeStatus status{DecodeStatus::kOk};
  // Prefix safe to keep when repairing an interrupted append before restarting.
  std::size_t valid_bytes{};
  std::size_t error_offset{};
};

namespace decode_internal {
class BlockReader {
 public:
  BlockReader(std::span<std::uint8_t const> bytes, std::size_t declared,
              std::size_t prefix_size)
      : bytes_(bytes), declared_(declared), cursor_(prefix_size) {}

  template <typename Codec>
  bool Packed(std::uint64_t& value) {
    if (!ok()) return false;
    // Zero completion gives the smallest possible width consistent with the
    // bytes already visible. A declared block too short even for that width
    // is corrupt, including when its physical tail is also missing.
    std::array<std::uint8_t, Codec::kMaxWireBytes> prefix{};
    auto const available = std::min(prefix.size(), bytes_.size() - cursor_);
    std::copy_n(bytes_.begin() + cursor_, available, prefix.begin());
    auto const minimum = Codec::WireBytesNeeded(prefix.data(), prefix.size());
    if (minimum > declared_ - cursor_) return Malformed();
    auto const status = wire::Read<Codec>(bytes_, cursor_, value);
    return Accept(status);
  }
  bool U64(std::uint64_t& value) {
    if (!ok()) return false;
    if (8 > declared_ - cursor_) return Malformed();
    return Accept(wire::ReadU64(bytes_, cursor_, value));
  }
  bool Byte(std::uint8_t& value) {
    if (!ok()) return false;
    if (cursor_ >= declared_) return Malformed();
    if (cursor_ >= bytes_.size()) return Missing();
    value = bytes_[cursor_++];
    return true;
  }
  bool Bytes(std::uint64_t size, std::span<std::uint8_t const>& value) {
    if (!ok()) return false;
    if (size > declared_ - cursor_) return Malformed();
    if (size > bytes_.size() - cursor_) return Missing();
    value = bytes_.subspan(cursor_, static_cast<std::size_t>(size));
    cursor_ += static_cast<std::size_t>(size);
    return true;
  }
  bool Malformed() {
    status_ = DecodeStatus::kMalformed;
    return false;
  }
  bool ok() const { return status_ == DecodeStatus::kOk; }
  std::size_t cursor() const { return cursor_; }
  DecodeStatus status() const { return status_; }

 private:
  bool Missing() {
    status_ = bytes_.size() < declared_ ? DecodeStatus::kTruncated
                                        : DecodeStatus::kMalformed;
    return false;
  }
  bool Accept(wire::ReadStatus status) {
    if (status == wire::ReadStatus::kOk) return true;
    return status == wire::ReadStatus::kTruncated ? Missing() : Malformed();
  }
  std::span<std::uint8_t const> bytes_;
  std::size_t declared_;
  std::size_t cursor_{};
  DecodeStatus status_{DecodeStatus::kOk};
};

// A block is the persistence unit. Do not expose any of its rows until its
// entire declared byte range has arrived and every row has been validated.
inline DecodeStatus ReadBlock(std::span<std::uint8_t const> bytes,
                              std::size_t size, std::size_t prefix_size,
                              std::vector<DecodedRecord>& records) {
  BlockReader reader(bytes, size, prefix_size);
  std::uint8_t control{};
  if (!reader.Byte(control)) return reader.status();
  auto const fields = wire::Fields(control);
  auto const kind = control & 3;
  bool const wide = (control & wire::kWide) != 0;
  if (!wire::ValidControl(control)) return DecodeStatus::kMalformed;
  std::uint64_t stream{}, sequence{}, time{};
  if (!reader.Packed<wire::StreamCodec>(stream)) return reader.status();
  if (stream > wire::kMaxStream) return DecodeStatus::kMalformed;
  if (!(wide ? reader.U64(sequence)
             : reader.Packed<wire::SequenceCodec>(sequence)))
    return reader.status();
  if ((fields & kTime) != 0 && !reader.U64(time)) return reader.status();
  bool any = false;
  while (reader.cursor() < size) {
    DecodedRecord record;
    record.stream = static_cast<std::uint32_t>(stream);
    record.fields = fields;
    record.kind = kind == wire::kLogBlock ? Kind::kLog : Kind::kMetric;
    record.sequence = sequence;
    std::uint64_t index{};
    if (!reader.Packed<wire::TagCodec>(index)) return reader.status();
    if (index > wire::kMaxTag) return DecodeStatus::kMalformed;
    record.index = static_cast<std::uint32_t>(index);
    if (kind == wire::kMetricBlock) {
      Metric metric;
      bool const ok =
          wide ? reader.U64(metric.count) && reader.U64(metric.samples) &&
                     reader.U64(metric.sum) && reader.U64(metric.min) &&
                     reader.U64(metric.max)
               : reader.Packed<wire::CountCodec>(metric.count) &&
                     reader.Packed<wire::CountCodec>(metric.samples) &&
                     reader.Packed<wire::SumCodec>(metric.sum) &&
                     reader.Packed<wire::SumCodec>(metric.min) &&
                     reader.Packed<wire::SumCodec>(metric.max);
      if (!ok) return reader.status();
      if (metric.samples != 0 && metric.min > metric.max)
        return DecodeStatus::kMalformed;
      record.metric = metric;
    } else {
      if ((fields & kTime) != 0) {
        std::uint64_t code{};
        if (!reader.Packed<wire::TimeCodec>(code)) return reader.status();
        if (code > 511) return DecodeStatus::kMalformed;
        auto const delta = wire::TimeValue(static_cast<std::uint16_t>(code));
        if (delta > std::numeric_limits<std::uint64_t>::max() - time)
          return DecodeStatus::kMalformed;
        time += delta;
        record.time = time;
      }
      if ((fields & kDuration) != 0) {
        if (!reader.Byte(record.duration_code)) return reader.status();
        auto const value = wire::DecodeDuration(record.duration_code);
        record.duration_state = value.state;
        if (value.state == DurationState::kValue)
          record.duration = value.microseconds;
      }
      if ((fields & kPayload) != 0) {
        std::uint64_t length{};
        if (!reader.Packed<wire::BlobCodec>(length) ||
            !reader.Bytes(length, record.payload))
          return reader.status();
      }
    }
    records.push_back(record);
    any = true;
    if (reader.cursor() < size) {
      if (sequence == std::numeric_limits<std::uint64_t>::max())
        return DecodeStatus::kMalformed;
      ++sequence;
    }
  }
  if (!any) return DecodeStatus::kMalformed;
  return bytes.size() < size ? DecodeStatus::kTruncated : DecodeStatus::kOk;
}
}  // namespace decode_internal

inline DecodeResult Decode(std::span<std::uint8_t const> input) {
  DecodeResult result;
  while (result.valid_bytes < input.size()) {
    auto const remaining = input.subspan(result.valid_bytes);
    result.error_offset = result.valid_bytes;
    std::size_t prefix_size = 0;
    std::uint64_t size = 0;
    auto const prefix_status =
        wire::Read<wire::BlockSizeCodec>(remaining, prefix_size, size);
    if (prefix_status != wire::ReadStatus::kOk) {
      result.status = prefix_status == wire::ReadStatus::kTruncated
                          ? DecodeStatus::kTruncated
                          : DecodeStatus::kMalformed;
      return result;
    }
    if (size < kMinBlockSize || size < prefix_size) {
      result.status = DecodeStatus::kMalformed;
      return result;
    }
    std::vector<DecodedRecord> block;
    result.status = decode_internal::ReadBlock(
        remaining.first(std::min<std::size_t>(size, remaining.size())), size,
        prefix_size, block);
    if (result.status != DecodeStatus::kOk) return result;
    result.records.insert(result.records.end(), block.begin(), block.end());
    result.valid_bytes += size;
  }
  result.error_offset = result.valid_bytes;
  return result;
}
struct Reconstruction {
  std::vector<DecodedRecord> records;
  bool all_timestamps_present{true};
  bool clock_regressed{false};
  bool sequence_reset{false};
};

// Input captures must be supplied in append order, without overlapping bytes.
// Preserve that order for each writer, including sequence resets after restart,
// then merge available writer heads by time. Ordinary timestamped lifecycle
// events provide context; no special restart identifier or marker is required.
// Without timestamps only per-writer order is known; tie order is
// deterministic.
inline Reconstruction Reconstruct(std::vector<DecodedRecord> const& input) {
  Reconstruction result;
  std::map<std::uint32_t, std::vector<DecodedRecord>> streams;
  for (auto const& record : input) {
    streams[record.stream].push_back(record);
    if (!record.time) result.all_timestamps_present = false;
  }
  struct Cursor {
    std::vector<DecodedRecord> const* records;
    std::size_t next{};
  };
  std::vector<Cursor> cursors;
  for (auto& [key, records] : streams) {
    (void)key;
    for (std::size_t i = 1; i < records.size(); ++i) {
      auto const& previous = records[i - 1];
      auto const& current = records[i];
      if (current.sequence <= previous.sequence) result.sequence_reset = true;
      if (previous.time && current.time && *current.time < *previous.time)
        result.clock_regressed = true;
    }
    cursors.push_back({&records, 0});
  }
  auto& merged = result.records;
  merged.reserve(input.size());
  while (merged.size() < input.size()) {
    Cursor* selected = nullptr;
    for (auto& cursor : cursors) {
      if (cursor.next == cursor.records->size()) continue;
      if (selected == nullptr) {
        selected = &cursor;
        continue;
      }
      auto const& a = (*cursor.records)[cursor.next];
      auto const& b = (*selected->records)[selected->next];
      bool earlier = (a.time && b.time && *a.time != *b.time)
                         ? *a.time < *b.time
                         : a.stream < b.stream;
      if (earlier) selected = &cursor;
    }
    merged.push_back((*selected->records)[selected->next++]);
  }
  return result;
}

inline std::vector<DecodedRecord> MergeRecords(
    std::vector<DecodedRecord> const& input) {
  return Reconstruct(input).records;
}

// Metric frames carry disjoint acknowledged deltas, not cumulative snapshots.
// Supply every persisted byte exactly once. Sequence numbers can reset, so they
// are not persistent identities and cannot be used to deduplicate captures.
inline std::map<std::uint32_t, Metric> RestoreMetrics(
    std::vector<DecodedRecord> const& records) {
  std::map<std::uint32_t, Metric> result;
  for (auto const& record : records) {
    if (!record.metric) continue;
    AccumulateMetric(result[record.index], *record.metric);
  }
  return result;
}

inline Metric RestoreMetric(std::vector<DecodedRecord> const& records,
                            std::uint32_t index) {
  auto metrics = RestoreMetrics(records);
  auto it = metrics.find(index);
  return it == metrics.end() ? Metric{} : it->second;
}

inline void PrintRecord(std::ostream& out, DecodedRecord const& record) {
  out << "stream=" << record.stream << " sequence=" << record.sequence
      << " index=" << record.index;
  if (record.time) out << " time_us=" << *record.time;
  if (record.duration)
    out << " duration_us=" << *record.duration;
  else if (record.fields & kDuration) {
    switch (record.duration_state) {
      case DurationState::kBelowMin:
        out << " duration=<min";
        break;
      case DurationState::kAboveMax:
        out << " duration=>max";
        break;
      default:
        out << " duration=unknown";
        break;
    }
  }
  if (record.fields & kPayload)
    out << " payload_bytes=" << record.payload.size();
  if (record.metric) {
    auto const& metric = *record.metric;
    out << " count=" << metric.count << " samples=" << metric.samples
        << " sum_us=" << metric.sum << " min_us=" << metric.min
        << " max_us=" << metric.max;
  }
  out << '\n';
}

}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_DECODE_H_
