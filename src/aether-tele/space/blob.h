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

#ifndef AETHER_TELE_SPACE_BLOB_H_
#define AETHER_TELE_SPACE_BLOB_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aether-tele/env/library_version.h"
#include "aether-tele/space/codec.h"
#include "aether-tele/space/schema.h"
#include "aether-tele/space/storage.h"

namespace ae::tele::space {

inline constexpr std::uint8_t kFlagHasLibraryVersion = 1 << 0;

enum class BlobStatus {
  kOk = 0,
  kBadMagic,
  kChecksumMismatch,
  kTruncated,
  kInvalidRecord,
  kBadVersion,
  kUnknownSection,
  kCorruptHeader,
};

struct DecodedEvent {
  std::uint32_t index{};
  StreamMarker stream{StreamMarker::kMain};
  std::uint64_t delta{};
  std::uint64_t rel_time{};
  std::uint64_t duration{};
  bool has_duration{false};
  std::uint64_t payload0{};
  std::uint64_t payload1{};
  std::size_t payload_fields{};
  std::size_t index_bytes{};
  std::size_t delta_bytes{};
  std::size_t record_bytes{};
#if defined(AE_TELE_SPACE_HOST)
  std::string_view name{};
  std::string_view module{};
  std::uint8_t severity{};
#endif
};

struct DecodedMetric {
  std::uint32_t index{};
  std::uint64_t count{};
  std::uint64_t sum{};
  std::uint64_t min{};
  std::uint64_t max{};
#if defined(AE_TELE_SPACE_HOST)
  std::string_view name{};
#endif
};

struct DecodedStream {
  StreamMarker marker{StreamMarker::kMain};
  std::size_t log_size{};
  std::vector<DecodedEvent> events;
};

template <typename Config>
struct DecodedBlob {
  std::uint32_t checksum{};
  std::uint32_t sequence{};
  std::uint32_t base_unix{};
  std::string library_version;
  std::size_t blob_size{};
  std::size_t header_size{};
  std::size_t stream_section_size{};
  std::size_t metrics_section_size{};
  std::size_t event_count{};
  std::array<std::size_t, 9> index_size_hist{};
  std::array<std::size_t, 9> delta_size_hist{};
  std::vector<DecodedStream> streams;
  std::vector<DecodedEvent> events;  // merged by rel_time, then marker
  std::vector<DecodedMetric> metrics;
};

struct DecodeOutcomeHeader {
  BlobStatus status{BlobStatus::kTruncated};
  std::uint32_t blob_checksum{};
  std::uint32_t expected_checksum{};
};

template <typename Config>
struct DecodeOutcome : DecodeOutcomeHeader {
  std::optional<DecodedBlob<Config>> data;
};

namespace blob_internal {

inline void AppendRaw(std::vector<std::uint8_t>& out, std::uint8_t const* p,
                      std::size_t n) {
  out.insert(out.end(), p, p + n);
}

template <typename Codec>
inline void AppendEncoded(std::vector<std::uint8_t>& out, std::uint64_t value) {
  std::array<std::uint8_t, Codec::kMaxEncodedSize> buf{};
  auto const n = Codec::Encode(value, buf.data());
  AppendRaw(out, buf.data(), n);
}

inline bool Consume(std::uint8_t const*& p, std::uint8_t const* end,
                    std::size_t n, std::uint8_t const*& out) {
  if (p + n > end) {
    return false;
  }
  out = p;
  p += n;
  return true;
}

template <typename Codec>
bool ReadValue(std::uint8_t const*& p, std::uint8_t const* end,
               std::uint64_t& value, std::size_t& bytes) {
  if (p >= end) {
    return false;
  }
  auto const decoded = Codec::Decode(p, static_cast<std::size_t>(end - p));
  if (decoded.bytes_read == 0 || p + decoded.bytes_read > end) {
    return false;
  }
  value = decoded.value;
  bytes = decoded.bytes_read;
  p += decoded.bytes_read;
  return true;
}

}  // namespace blob_internal

template <typename Config>
std::vector<std::uint8_t> SerializeBlob(TeleStorage<Config> const& storage) {
  using CountCodec = typename Config::CountCodec;
  using TimeCodec = typename Config::TimeCodec;
  using IndexCodec = typename Config::IndexCodec;

  std::vector<std::uint8_t> stream_section;
  std::uint64_t stream_count = 0;
  for (std::size_t i = 0; i < TeleStorage<Config>::kMaxStreams; ++i) {
    auto const marker = static_cast<StreamMarker>(static_cast<std::uint8_t>(i));
    if (storage.stream_bound(marker)) {
      ++stream_count;
    }
  }
  blob_internal::AppendEncoded<CountCodec>(stream_section, stream_count);
  for (std::size_t i = 0; i < TeleStorage<Config>::kMaxStreams; ++i) {
    auto const marker = static_cast<StreamMarker>(static_cast<std::uint8_t>(i));
    if (!storage.stream_bound(marker)) {
      continue;
    }
    auto const bytes = storage.StreamBytes(marker);
    blob_internal::AppendEncoded<CountCodec>(stream_section, i);
    blob_internal::AppendEncoded<CountCodec>(stream_section, bytes.size());
    blob_internal::AppendRaw(stream_section, bytes.data(), bytes.size());
  }

  std::vector<std::uint8_t> metrics_section;
  std::uint64_t metric_n = 0;
  for (std::size_t t = 0; t < Config::kTags.size(); ++t) {
    auto const m = storage.MergedMetric(t);
    if (m.count != typename Config::CountType{} || m.has_duration) {
      ++metric_n;
    }
  }
  blob_internal::AppendEncoded<CountCodec>(metrics_section, metric_n);
  for (std::size_t t = 0; t < Config::kTags.size(); ++t) {
    auto const m = storage.MergedMetric(t);
    if (m.count == typename Config::CountType{} && !m.has_duration) {
      continue;
    }
    blob_internal::AppendEncoded<IndexCodec>(metrics_section,
                                             Config::kTags[t].index);
    blob_internal::AppendEncoded<CountCodec>(
        metrics_section, static_cast<std::uint64_t>(m.count));
    blob_internal::AppendEncoded<TimeCodec>(metrics_section,
                                            static_cast<std::uint64_t>(m.sum));
    blob_internal::AppendEncoded<TimeCodec>(metrics_section,
                                            static_cast<std::uint64_t>(m.min));
    blob_internal::AppendEncoded<TimeCodec>(metrics_section,
                                            static_cast<std::uint64_t>(m.max));
  }

  std::string_view const library_version = LIBRARY_VERSION;
  std::uint8_t flags = 0;
  if (!library_version.empty()) {
    flags = kFlagHasLibraryVersion;
  }

  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
  std::array<std::uint8_t, 4> checksum_bytes{};
  WriteU32LE(checksum_bytes.data(), Config::kSchemaChecksum);
  blob_internal::AppendRaw(out, checksum_bytes.data(), 4);
  out.push_back(kSpaceFormatVersion);
  out.push_back(flags);
  blob_internal::AppendEncoded<CountCodec>(out, storage.sequence());
  std::array<std::uint8_t, 4> unix_bytes{};
  WriteU32LE(unix_bytes.data(), storage.base_unix());
  blob_internal::AppendRaw(out, unix_bytes.data(), 4);
  blob_internal::AppendEncoded<CountCodec>(out, stream_section.size());
  blob_internal::AppendEncoded<CountCodec>(out, metrics_section.size());
  if ((flags & kFlagHasLibraryVersion) != 0) {
    blob_internal::AppendEncoded<CountCodec>(out, library_version.size());
    blob_internal::AppendRaw(
        out, reinterpret_cast<std::uint8_t const*>(library_version.data()),
        library_version.size());
  }
  blob_internal::AppendRaw(out, stream_section.data(), stream_section.size());
  blob_internal::AppendRaw(out, metrics_section.data(), metrics_section.size());
  return out;
}

template <typename Config>
DecodeOutcome<Config> DecodeBlob(std::span<std::uint8_t const> blob) {
  using CountCodec = typename Config::CountCodec;
  using TimeCodec = typename Config::TimeCodec;
  using DeltaCodec = typename Config::DeltaCodec;
  using IndexCodec = typename Config::IndexCodec;

  DecodeOutcome<Config> outcome;
  outcome.expected_checksum = Config::kSchemaChecksum;
  if (blob.size() < 10) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  if (blob[0] != kMagic[0] || blob[1] != kMagic[1] || blob[2] != kMagic[2] ||
      blob[3] != kMagic[3]) {
    outcome.status = BlobStatus::kBadMagic;
    return outcome;
  }

  outcome.blob_checksum = ReadU32LE(blob.data() + 4);
  if (outcome.blob_checksum != Config::kSchemaChecksum) {
    outcome.status = BlobStatus::kChecksumMismatch;
    return outcome;
  }

  auto const* p = blob.data() + 8;
  auto const* end = blob.data() + blob.size();
  if (p >= end) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  if (*p != kSpaceFormatVersion) {
    outcome.status = BlobStatus::kBadVersion;
    return outcome;
  }
  ++p;
  if (p >= end) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  auto const flags = *p;
  ++p;

  std::uint64_t sequence = 0;
  std::size_t nread = 0;
  if (!blob_internal::ReadValue<CountCodec>(p, end, sequence, nread)) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  if (p + 4 > end) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  auto const base_unix = ReadU32LE(p);
  p += 4;

  std::uint64_t stream_size = 0;
  std::uint64_t metrics_size = 0;
  if (!blob_internal::ReadValue<CountCodec>(p, end, stream_size, nread) ||
      !blob_internal::ReadValue<CountCodec>(p, end, metrics_size, nread)) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }

  std::string library_version;
  if ((flags & kFlagHasLibraryVersion) != 0) {
    std::uint64_t lib_len = 0;
    if (!blob_internal::ReadValue<CountCodec>(p, end, lib_len, nread)) {
      outcome.status = BlobStatus::kTruncated;
      return outcome;
    }
    std::uint8_t const* lib_ptr = nullptr;
    if (!blob_internal::Consume(p, end, static_cast<std::size_t>(lib_len),
                                lib_ptr)) {
      outcome.status = BlobStatus::kTruncated;
      return outcome;
    }
    library_version.assign(reinterpret_cast<char const*>(lib_ptr),
                           static_cast<std::size_t>(lib_len));
  }

  std::uint8_t const* stream_ptr = nullptr;
  std::uint8_t const* metrics_ptr = nullptr;
  if (!blob_internal::Consume(p, end, static_cast<std::size_t>(stream_size),
                              stream_ptr) ||
      !blob_internal::Consume(p, end, static_cast<std::size_t>(metrics_size),
                              metrics_ptr)) {
    outcome.status = BlobStatus::kTruncated;
    return outcome;
  }
  if (p != end) {
    outcome.status = BlobStatus::kUnknownSection;
    return outcome;
  }

  DecodedBlob<Config> decoded;
  decoded.checksum = outcome.blob_checksum;
  decoded.sequence = static_cast<std::uint32_t>(sequence);
  decoded.base_unix = base_unix;
  decoded.library_version = std::move(library_version);
  decoded.blob_size = blob.size();
  decoded.stream_section_size = static_cast<std::size_t>(stream_size);
  decoded.metrics_section_size = static_cast<std::size_t>(metrics_size);
  decoded.header_size =
      blob.size() - decoded.stream_section_size - decoded.metrics_section_size;

  auto sp = stream_ptr;
  auto const stream_end = stream_ptr + decoded.stream_section_size;
  std::uint64_t stream_count = 0;
  if (!blob_internal::ReadValue<CountCodec>(sp, stream_end, stream_count,
                                            nread)) {
    outcome.status = BlobStatus::kInvalidRecord;
    return outcome;
  }

  auto parse_log = [&](StreamMarker marker, std::uint8_t const* log_ptr,
                       std::size_t log_size,
                       std::vector<DecodedEvent>& events) -> bool {
    auto lp = log_ptr;
    auto const log_end = log_ptr + log_size;
    std::uint64_t rel = 0;
    while (lp < log_end) {
      auto const* rec_begin = lp;
      std::uint64_t index = 0;
      std::size_t index_bytes = 0;
      if (!blob_internal::ReadValue<IndexCodec>(lp, log_end, index,
                                                index_bytes)) {
        return false;
      }
      auto const* tag =
          FindTag(Config::kTags, static_cast<std::uint32_t>(index));
      if (tag == nullptr) {
        return false;
      }
      DecodedEvent event;
      event.index = static_cast<std::uint32_t>(index);
      event.stream = marker;
#if defined(AE_TELE_SPACE_HOST)
      if constexpr (requires { Config::kHostTags; }) {
        if (auto const* host =
                FindHostTag(Config::kHostTags, event.index)) {
          event.name = host->name;
          event.module = host->module;
          event.severity = host->severity;
        }
      }
#endif
      std::size_t delta_bytes = 0;
      if (tag->timed()) {
        std::uint64_t delta = 0;
        if (!blob_internal::ReadValue<DeltaCodec>(lp, log_end, delta,
                                                  delta_bytes)) {
          return false;
        }
        event.delta = delta;
        rel += delta;
      }
      event.rel_time = rel;
      if (tag->payload == PayloadKind::kPackedU32 ||
          tag->payload == PayloadKind::kPackedU64) {
        std::uint64_t field = 0;
        std::size_t dummy = 0;
        if (!blob_internal::ReadValue<CountCodec>(lp, log_end, field, dummy)) {
          return false;
        }
        event.payload0 = field;
        event.payload_fields = 1;
      } else if (tag->payload == PayloadKind::kPackedU32U32) {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::size_t dummy = 0;
        if (!blob_internal::ReadValue<CountCodec>(lp, log_end, a, dummy) ||
            !blob_internal::ReadValue<CountCodec>(lp, log_end, b, dummy)) {
          return false;
        }
        event.payload0 = a;
        event.payload1 = b;
        event.payload_fields = 2;
      }
      if (tag->has_duration()) {
        std::uint64_t duration = 0;
        std::size_t dummy = 0;
        if (!blob_internal::ReadValue<TimeCodec>(lp, log_end, duration,
                                                 dummy)) {
          return false;
        }
        event.duration = duration;
        event.has_duration = true;
      }
      event.index_bytes = index_bytes;
      event.delta_bytes = delta_bytes;
      event.record_bytes = static_cast<std::size_t>(lp - rec_begin);
      if (index_bytes < decoded.index_size_hist.size()) {
        decoded.index_size_hist[index_bytes] += 1;
      }
      if (delta_bytes < decoded.delta_size_hist.size()) {
        decoded.delta_size_hist[delta_bytes] += 1;
      }
      events.push_back(event);
    }
    return lp == log_end;
  };

  for (std::uint64_t s = 0; s < stream_count; ++s) {
    std::uint64_t marker_v = 0;
    std::uint64_t log_size = 0;
    if (!blob_internal::ReadValue<CountCodec>(sp, stream_end, marker_v,
                                              nread) ||
        !blob_internal::ReadValue<CountCodec>(sp, stream_end, log_size,
                                              nread)) {
      outcome.status = BlobStatus::kInvalidRecord;
      return outcome;
    }
    std::uint8_t const* log_ptr = nullptr;
    if (!blob_internal::Consume(sp, stream_end,
                                static_cast<std::size_t>(log_size), log_ptr)) {
      outcome.status = BlobStatus::kInvalidRecord;
      return outcome;
    }
    DecodedStream stream;
    stream.marker = static_cast<StreamMarker>(static_cast<std::uint8_t>(marker_v));
    stream.log_size = static_cast<std::size_t>(log_size);
    if (!parse_log(stream.marker, log_ptr, stream.log_size, stream.events)) {
      outcome.status = BlobStatus::kInvalidRecord;
      return outcome;
    }
    decoded.event_count += stream.events.size();
    decoded.streams.push_back(std::move(stream));
  }
  if (sp != stream_end) {
    outcome.status = BlobStatus::kInvalidRecord;
    return outcome;
  }

  auto mp = metrics_ptr;
  auto const metrics_end = metrics_ptr + decoded.metrics_section_size;
  std::uint64_t metric_count = 0;
  if (decoded.metrics_section_size > 0) {
    if (!blob_internal::ReadValue<CountCodec>(mp, metrics_end, metric_count,
                                              nread)) {
      outcome.status = BlobStatus::kInvalidRecord;
      return outcome;
    }
    for (std::uint64_t i = 0; i < metric_count; ++i) {
      std::uint64_t index = 0;
      std::uint64_t count = 0;
      std::uint64_t sum = 0;
      std::uint64_t minv = 0;
      std::uint64_t maxv = 0;
      std::size_t dummy = 0;
      if (!blob_internal::ReadValue<IndexCodec>(mp, metrics_end, index,
                                                dummy) ||
          !blob_internal::ReadValue<CountCodec>(mp, metrics_end, count,
                                                dummy) ||
          !blob_internal::ReadValue<TimeCodec>(mp, metrics_end, sum, dummy) ||
          !blob_internal::ReadValue<TimeCodec>(mp, metrics_end, minv, dummy) ||
          !blob_internal::ReadValue<TimeCodec>(mp, metrics_end, maxv, dummy)) {
        outcome.status = BlobStatus::kInvalidRecord;
        return outcome;
      }
      DecodedMetric metric;
      metric.index = static_cast<std::uint32_t>(index);
      metric.count = count;
      metric.sum = sum;
      metric.min = minv;
      metric.max = maxv;
#if defined(AE_TELE_SPACE_HOST)
      if constexpr (requires { Config::kHostTags; }) {
        if (auto const* host = FindHostTag(Config::kHostTags, metric.index)) {
          metric.name = host->name;
        }
      }
#endif
      decoded.metrics.push_back(metric);
    }
    if (mp != metrics_end) {
      outcome.status = BlobStatus::kInvalidRecord;
      return outcome;
    }
  }

  for (auto const& stream : decoded.streams) {
    decoded.events.insert(decoded.events.end(), stream.events.begin(),
                          stream.events.end());
  }
  std::sort(decoded.events.begin(), decoded.events.end(),
            [](DecodedEvent const& a, DecodedEvent const& b) {
              if (a.rel_time != b.rel_time) {
                return a.rel_time < b.rel_time;
              }
              return static_cast<std::uint8_t>(a.stream) <
                     static_cast<std::uint8_t>(b.stream);
            });

  outcome.status = BlobStatus::kOk;
  outcome.data = std::move(decoded);
  return outcome;
}

inline bool BlobContainsText(std::span<std::uint8_t const> blob,
                             std::string_view text) {
  if (text.empty() || blob.size() < text.size()) {
    return false;
  }
  auto const* begin = reinterpret_cast<char const*>(blob.data());
  std::string_view view{begin, blob.size()};
  return view.find(text) != std::string_view::npos;
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_BLOB_H_
