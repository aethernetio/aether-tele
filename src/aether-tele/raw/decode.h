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
  // Number of bytes consumed by the record reader. No tail repair is performed.
  std::size_t valid_bytes{};
  std::size_t error_offset{};
};

// State belongs to the byte stream, not to a drain callback or a RAM batch.
struct DecodeState {
  std::map<std::uint32_t, std::uint64_t> time;
};
inline DecodeResult Decode(std::span<std::uint8_t const> input,
                           DecodeState& state) {
  DecodeResult result;
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
    result.error_offset = cursor;
    std::uint64_t index{}, stream{};
    if (!packed.template operator()<wire::TagCodec>(index)) return fail();
    if (index == 0) {
      std::uint64_t absolute{};
      if (!packed.template operator()<wire::StreamCodec>(stream) ||
          !u64(absolute))
        return fail();
      state.time[static_cast<std::uint32_t>(stream)] = absolute;
      result.valid_bytes = cursor;
      continue;
    }
    if (cursor == input.size()) return fail();
    auto const control = input[cursor++];
    if (!wire::ValidControl(control)) return fail();
    bool const wide = (control & wire::kWide) != 0;
    DecodedRecord record;
    record.index = static_cast<std::uint32_t>(index);
    record.fields = wire::Fields(control);
    record.kind = (control & 3) == 0 ? Kind::kLog : Kind::kMetric;
    if (!packed.template operator()<wire::StreamCodec>(stream)) return fail();
    record.stream = static_cast<std::uint32_t>(stream);
    if (!(wide ? u64(record.sequence)
               : packed.template operator()<wire::SequenceCodec>(
                     record.sequence)))
      return fail();
    if (record.kind == Kind::kMetric) {
      Metric m;
      bool ok =
          wide ? u64(m.count) && u64(m.samples) && u64(m.sum) && u64(m.min) &&
                     u64(m.max)
               : packed.template operator()<wire::CountCodec>(m.count) &&
                     packed.template operator()<wire::CountCodec>(m.samples) &&
                     packed.template operator()<wire::SumCodec>(m.sum) &&
                     packed.template operator()<wire::SumCodec>(m.min) &&
                     packed.template operator()<wire::SumCodec>(m.max);
      if (!ok) return fail();
      record.metric = m;
    } else {
      if (record.fields & kTime) {
        std::uint64_t delta{};
        auto found = state.time.find(record.stream);
        if (found == state.time.end() ||
            !packed.template operator()<wire::TimeCodec>(delta))
          return fail();
        found->second += wire::TimeValue(static_cast<std::uint16_t>(delta));
        record.time = found->second;
      }
      if (record.fields & kDuration) {
        if (cursor == input.size()) return fail();
        record.duration_code = input[cursor++];
        auto duration = wire::DecodeDuration(record.duration_code);
        record.duration_state = duration.state;
        if (duration.known()) record.duration = duration.microseconds;
      }
      if (record.fields & kPayload) {
        std::uint64_t length{};
        if (!packed.template operator()<wire::BlobCodec>(length) ||
            length > input.size() - cursor)
          return fail();
        record.payload =
            input.subspan(cursor, static_cast<std::size_t>(length));
        cursor += static_cast<std::size_t>(length);
      }
    }
    result.records.push_back(record);
    result.valid_bytes = cursor;
  }
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
