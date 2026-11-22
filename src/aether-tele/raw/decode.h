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
  // Decoder-derived append ordinal for source compatibility; never on the wire.
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
  // Number of bytes consumed by the record reader. No tail repair is performed.
  std::size_t valid_bytes{};
  std::size_t error_offset{};
};

// Each block is self-contained. State only supplies derived per-writer append
// ordinals across successive calls; timestamps restart from each writer header.
struct DecodeState {
  std::map<std::uint32_t, std::uint64_t> time, ordinal;
};
inline DecodeResult Decode(std::span<std::uint8_t const> input,
                           DecodeState& state) {
  DecodeResult result;
  auto next = state;
  std::size_t cursor = 0;
  auto packed = [&]<typename Codec>(std::uint64_t& value) {
    return wire::Read<Codec>(input, cursor, value) == wire::ReadStatus::kOk;
  };
  auto u64 = [&](std::uint64_t& value) {
    return wire::ReadU64(input, cursor, value) == wire::ReadStatus::kOk;
  };
  auto fail = [&]() {
    result.status = DecodeStatus::kMalformed;
    return result;
  };
  while (cursor < input.size()) {
    auto const block_start = cursor;
    result.error_offset = cursor;
    auto const kind = input[cursor++];
    if (kind > wire::kMetricRecord) return fail();
    std::uint64_t writers{};
    if (!packed.template operator()<wire::StreamCodec>(writers) || !writers ||
        writers > input.size() - cursor)
      return fail();
    std::uint32_t fields = 0;
    if (kind == wire::kLogRecord) {
      if (cursor == input.size()) return fail();
      fields = input[cursor++];
      if (fields & ~kAllFields) return fail();
    }
    std::vector<std::uint32_t> seen;
    for (std::uint64_t writer = 0; writer < writers; ++writer) {
      std::uint64_t stream{}, bytes{}, count{}, time{};
      if (!packed.template operator()<wire::StreamCodec>(stream) ||
          !packed.template operator()<wire::BlobCodec>(bytes) ||
          !packed.template operator()<wire::MetricCodec>(count) || !bytes ||
          !count || count > bytes)
        return fail();
      auto const id = static_cast<std::uint32_t>(stream);
      if (std::find(seen.begin(), seen.end(), id) != seen.end()) return fail();
      seen.push_back(id);
      if ((fields & kTime) && !u64(time)) return fail();
      if (bytes > input.size() - cursor) return fail();
      auto const end = cursor + static_cast<std::size_t>(bytes);
      // Restrict every read to this writer body: malformed lengths cannot
      // consume bytes from a following writer or telemetry block.
      auto body_packed = [&]<typename Codec>(std::uint64_t& value) {
        return wire::Read<Codec>(input.first(end), cursor, value) ==
               wire::ReadStatus::kOk;
      };
      for (std::uint64_t item = 0; item < count;) {
        result.error_offset = cursor;
        std::uint64_t index{};
        if (!body_packed.template operator()<wire::TagCodec>(index))
          return fail();
        if (!index) {
          if (!(fields & kTime) ||
              wire::ReadU64(input.first(end), cursor, time) !=
                  wire::ReadStatus::kOk ||
              cursor == end)
            return fail();
          continue;
        }
        DecodedRecord record;
        record.index = static_cast<std::uint32_t>(index);
        record.stream = id;
        record.fields = fields;
        record.kind = kind == wire::kLogRecord ? Kind::kLog : Kind::kMetric;
        record.sequence = next.ordinal[id]++;
        if (record.kind == Kind::kMetric) {
          Metric m;
          if (!body_packed.template operator()<wire::MetricCodec>(m.count) ||
              !body_packed.template operator()<wire::MetricCodec>(m.samples) ||
              !body_packed.template operator()<wire::MetricCodec>(m.sum) ||
              !body_packed.template operator()<wire::MetricCodec>(m.min) ||
              !body_packed.template operator()<wire::MetricCodec>(m.max) ||
              (m.samples && m.min > m.max))
            return fail();
          record.metric = m;
        } else {
          if (fields & kTime) {
            std::uint64_t code{};
            if (!body_packed.template operator()<wire::TimeCodec>(code))
              return fail();
            auto delta = wire::TimeValue(static_cast<std::uint16_t>(code));
            if (delta > UINT64_MAX - time) return fail();
            time += delta;
            record.time = time;
          }
          if (fields & kDuration) {
            if (cursor == end) return fail();
            record.duration_code = input[cursor++];
            auto duration = wire::DecodeDuration(record.duration_code);
            record.duration_state = duration.state;
            if (duration.known()) record.duration = duration.microseconds;
          }
          if (fields & kPayload) {
            std::uint64_t length{};
            if (!body_packed.template operator()<wire::BlobCodec>(length) ||
                length > end - cursor)
              return fail();
            record.payload =
                input.subspan(cursor, static_cast<std::size_t>(length));
            cursor += static_cast<std::size_t>(length);
          }
        }
        result.records.push_back(record);
        ++item;
      }
      if (cursor != end) return fail();
      if (fields & kTime) next.time[id] = time;
    }
    result.valid_bytes = cursor;
    result.error_offset = block_start;
  }
  state = std::move(next);
  result.error_offset = cursor;
  return result;
}
inline DecodeResult Decode(std::span<std::uint8_t const> input) {
  DecodeState state;
  return Decode(input, state);
}
struct Reconstruction {
  std::vector<DecodedRecord> records;
  bool all_timestamps_present{true};
  bool clock_regressed{false};
  bool sequence_reset{false};
};

// Input captures must be supplied in append order, without overlapping bytes.
// Preserve physical append order for each writer across process restarts,
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
// Supply every persisted byte exactly once. There is no on-wire
// sequence number and repeated input ranges cannot be deduplicated.
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
  out << "stream=" << record.stream << " index=" << record.index;
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
