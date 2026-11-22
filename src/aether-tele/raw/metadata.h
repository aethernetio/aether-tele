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
#ifndef AETHER_TELE_RAW_METADATA_H_
#define AETHER_TELE_RAW_METADATA_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "aether-tele/raw/types.h"
#include "aether-tele/raw/wire.h"

#if defined(AE_TELE_RAW_DEBUG) && AE_TELE_RAW_DEBUG
#  include <array>
#  include <atomic>
#  include <ostream>
#  include <vector>
#else
#  include <iosfwd>
#endif

namespace ae::tele::raw {

#if defined(AE_TELE_RAW_DEBUG) && AE_TELE_RAW_DEBUG
namespace metadata_internal {
template <typename SpaceT>
bool SameLabels(Tag<SpaceT> const& a, Tag<SpaceT> const& b) {
  return a.name == b.name && a.text == b.text && a.module == b.module &&
         a.level == b.level;
}
inline void JsonString(std::ostream& out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out << '"';
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  out << '"';
}

template <typename SpaceT>
struct Sites {
  static constexpr std::size_t kCapacity = 1024;
  static inline std::array<Tag<SpaceT>, kCapacity> entries{};
  static inline std::size_t size{};
  static inline bool overflow{};
  static inline std::atomic_flag lock = ATOMIC_FLAG_INIT;
  struct Guard {
    Guard() {
      while (lock.test_and_set(std::memory_order_acquire)) {
      }
    }
    ~Guard() { lock.clear(std::memory_order_release); }
  };
};

template <typename SpaceT>
void WriteTag(std::ostream& out, Tag<SpaceT> const& tag) {
  out << "{\"index\":" << tag.index << ",\"name\":";
  JsonString(out, tag.name);
  out << ",\"text\":";
  JsonString(out, tag.text);
  out << ",\"file\":";
  JsonString(out, tag.file);
  out << ",\"line\":" << tag.line << ",\"module\":" << tag.module
      << ",\"level\":" << tag.level << '}';
}
}  // namespace metadata_internal

// Debug-only callsite capture. Invoke once per static callsite, not per event.
// Strings must have static lifetime. The explicit tag table remains
// authoritative and includes declarations whose event paths were never
// executed.
template <typename SpaceT>
bool RecordSite(Tag<SpaceT> const& tag, std::string_view file,
                std::uint32_t line) {
  using Sites = metadata_internal::Sites<SpaceT>;
  typename Sites::Guard guard;
  bool unique = true, known = false;
  for (std::size_t i = 0; i < Sites::size; ++i) {
    auto const& existing = Sites::entries[i];
    if (existing.index != tag.index) continue;
    bool const same = existing.file == file && existing.line == line &&
                      metadata_internal::SameLabels(existing, tag);
    unique = unique && same;
    known = known || same;
  }
  if (known) return unique;
  if (Sites::size == Sites::kCapacity) {
    Sites::overflow = true;
    return false;
  }
  auto copy = tag;
  copy.file = file;
  copy.line = line;
  Sites::entries[Sites::size++] = copy;
  // Keep conflicting sites too, so ExportMap cannot publish an ambiguous tag.
  return unique;
}

template <typename SpaceT>
bool ExportMap(std::ostream& out, std::string_view space,
               std::string_view build, std::span<Tag<SpaceT> const> tags) {
  // Never publish an ambiguous index table. Index reassignment is allowed
  // between builds; the matching application-build map must travel with data.
  if (!ValidUtf8(space) || !ValidUtf8(build)) return false;
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (tags[i].index == 0 || tags[i].index > 65535 ||
        !ValidUtf8(tags[i].name) || !ValidUtf8(tags[i].text) ||
        !ValidUtf8(tags[i].file))
      return false;
    for (std::size_t j = i + 1; j < tags.size(); ++j) {
      if (tags[i].index == tags[j].index) return false;
    }
  }
  using Sites = metadata_internal::Sites<SpaceT>;
  std::vector<Tag<SpaceT>> sites;
  bool complete;
  {
    typename Sites::Guard guard;
    sites.assign(Sites::entries.begin(), Sites::entries.begin() + Sites::size);
    complete = !Sites::overflow;
  }
  if (!complete) return false;
  // Only export callsites for the explicitly selected tag table.
  sites.erase(std::remove_if(sites.begin(), sites.end(),
                             [&](auto const& site) {
                               return std::none_of(tags.begin(), tags.end(),
                                                   [&](auto const& tag) {
                                                     return tag.index ==
                                                            site.index;
                                                   });
                             }),
              sites.end());
  for (std::size_t i = 0; i < sites.size(); ++i) {
    auto const& site = sites[i];
    if (!ValidUtf8(site.name) || !ValidUtf8(site.text) || !ValidUtf8(site.file))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (sites[j].index == site.index) return false;
    auto const declared =
        std::find_if(tags.begin(), tags.end(),
                     [&](auto const& tag) { return tag.index == site.index; });
    if (!metadata_internal::SameLabels(*declared, site)) return false;
  }
  out << "{\n\"space\":";
  metadata_internal::JsonString(out, space);
  out << ",\n\"application_build\":";
  metadata_internal::JsonString(out, build);
  out << ",\n\"byte_order\":\"little\",\n\"time_unit\":\"microsecond\","
         "\n\"layout\":\"thread_blocks\","
         "\n\"block_header\":[\"kind:u8\",\"writer_count:stream\",\"log_fields:"
         "u8?\"],"
         "\n\"writer_header\":[\"writer:stream\",\"body_bytes:blob\",\"records:"
         "metric\",\"base_time:u64?\"],"
         "\n\"absolute_time_tag\":0,"
         "\n\"absolute_time_record\":[\"tag=0\",\"u64_time_us\"],"
         "\n\"record\":[\"tag\",\"selected_fields\"],"
         "\n\"fields\":["
         "{\"bit\":1,\"name\":\"time_us\",\"type\":\"delta_time_code\"},"
         "{\"bit\":2,\"name\":\"duration_us\",\"type\":\"duration_code_u8\"},"
         "{\"bit\":8,\"name\":\"payload\",\"type\":\"blob_length_bytes\"}],"
         "\n\"kinds\":{\"0\":\"log\",\"1\":\"metric\"},"
         "\n\"numeric_codecs\":{"
         "\"tag\":{\"base_bytes\":1,\"thresholds\":[239,4095],\"max_value\":"
         "65535},"
         "\"stream\":{\"base_bytes\":1,\"thresholds\":[15,1023],\"max_value\":"
         "65535},"
         "\"metric\":{\"base_bytes\":1,\"thresholds\":[200,5000,100000000],"
         "\"max_value\":2182612529211367680},"
         "\"blob\":{\"base_bytes\":1,\"thresholds\":[250,1500]},"
         "\"time\":{\"base_bytes\":1,\"thresholds\":[250,511],\"max_value\":"
         "511}},"
         "\n\"exponential\":{\"fixed_point\":\"FixedPoint<uint32_t,64.0>\","
         "\"minimum_us\":1,\"maximum_us\":60000000,"
         "\"time_max_code\":511,\"duration_max_code\":253},"
         "\n\"duration_states\":{\"0\":\"below_min\",\"1\":\"minimum\","
         "\"253\":\"maximum\",\"254\":\"above_max\",\"255\":\"unknown\"},"
         "\n\"time_values_us\":[";
  bool separator = false;
  for (auto value : wire::TimeValues()) {
    if (separator) out << ',';
    separator = true;
    out << value;
  }
  out << "],\n\"duration_values_us\":[";
  separator = false;
  for (auto value : wire::DurationValues()) {
    if (separator) out << ',';
    separator = true;
    out << value;
  }
  out << "],\n\"log_row\":[\"tag\",\"time_code?\",\"duration_code?\","
         "\"blob_length?\",\"blob_bytes?\"],"
         "\n\"metric_body\":{"
         "\"fields\":[\"tag:tag\",\"count:metric\",\"samples:metric\","
         "\"sum_us:metric\",\"min_us:metric\",\"max_us:metric\"],"
         "\"aggregation\":\"acknowledged disjoint deltas, saturated at "
         "MetricCodec::kUpper\"},"
         "\n\"instructions\":\"Blocks concatenate. Each block declares its "
         "kind and writer count. "
         "Log field layout is shared by its writers, never repeated per "
         "record. "
         "Each writer declares its ID, body byte length and record count, plus "
         "an absolute time base for timed logs. "
         "Tag zero inside a timed writer resets that time base without "
         "repeating the writer ID. "
         "There are no per-record control, writer, sequence or wide fields. "
         "All metric values use the same TieredInt. Duration "
         "0=below,254=above,255=unknown.\","
         "\n\"tags\":[";
  bool first = true;
  for (auto const& tag : tags) {
    if (!first) out << ',';
    first = false;
    metadata_internal::WriteTag(out, tag);
  }
  out << "],\n\"observed_callsites\":[";
  first = true;
  for (auto const& tag : sites) {
    if (!first) out << ',';
    first = false;
    metadata_internal::WriteTag(out, tag);
  }
  out << "],\n\"callsite_capacity_exhausted\":" << (complete ? "false" : "true")
      << "\n}\n";
  return static_cast<bool>(out);
}
#else
template <typename SpaceT>
constexpr bool RecordSite(Tag<SpaceT> const&, std::string_view, std::uint32_t) {
  return true;
}
template <typename SpaceT>
bool ExportMap(std::ostream&, std::string_view, std::string_view,
               std::span<Tag<SpaceT> const>) {
  return false;
}
#endif

}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_METADATA_H_
