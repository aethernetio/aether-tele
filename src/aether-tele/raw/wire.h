/*
 * Copyright 2026 Aethernet Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at https://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#ifndef AETHER_TELE_RAW_WIRE_H_
#define AETHER_TELE_RAW_WIRE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(_MSC_VER)
// Numeric's signed/unsigned constexpr branches trigger optimized C4702 in
// MSVC. Keep the exception local to the external template definitions.
#  pragma warning(push)
#  pragma warning(disable : 4702)
#endif
#include "ae-numeric/exponential.h"
#include "ae-numeric/fixed_point.h"
#include "ae-numeric/tiered_int.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
#include "aether-tele/raw/types.h"

namespace ae::tele::raw::wire {

using TagCodec = ae::TieredInt<std::uint8_t, 239, 4095>;
using StreamCodec = ae::TieredInt<std::uint8_t, 15, 1023>;
using SequenceCodec = ae::TieredInt<std::uint16_t, 32767, 2'000'000'000>;
using CountCodec = ae::TieredInt<std::uint8_t, 247, 2047, 16'000'000>;
using SumCodec = SequenceCodec;
// Approved blob-length tiers: 0..250 -> 1 byte, 251..1500 -> 2 bytes,
// 1501..1'967'580 -> 4 bytes. The final maximum is derived by numeric.
using BlobCodec = ae::TieredInt<std::uint8_t, 250, 1500>;
using BlockSizeCodec = BlobCodec;
using TimeCodec = ae::TieredInt<std::uint8_t, 250, 511>;
using DurationCodec = ae::TieredInt<std::uint8_t, 253>;
using RuntimeSeconds = ae::FixedPoint<std::uint32_t, 64.0>;
using DurationExp =
    ae::Exponential<RuntimeSeconds, DurationCodec, 0.000001, 60.0, 253>;
using TimeExp = ae::Exponential<RuntimeSeconds, TimeCodec, 0.000001, 60.0, 511>;

// Keep bodies at a stable offset in RAM as the serialized size prefix grows.
// The unused leading bytes are never included in pending()/callbacks/files.
inline constexpr std::size_t kBlockPrefixReserve =
    BlockSizeCodec::kMaxWireBytes;
inline constexpr std::size_t kBlockPrefixSlack = kBlockPrefixReserve - 1;
inline constexpr std::uint8_t kLogBlock = 0;
inline constexpr std::uint8_t kMetricBlock = 1;
inline constexpr std::uint8_t kWide = 0x80;
inline constexpr std::uint32_t kMaxTag = 65535;
inline constexpr std::uint32_t kMaxStream = 65535;
inline constexpr std::uint64_t kDurationMinUs = 1;
inline constexpr std::uint64_t kDurationMaxUs = 60'000'000;
inline constexpr std::uint64_t kMaxTimeDeltaUs = 60'000'000;
inline constexpr std::uint8_t kDurationBelowMin = 0;
inline constexpr std::uint8_t kDurationMin = 1;
inline constexpr std::uint8_t kDurationMax = 253;
inline constexpr std::uint8_t kDurationAboveMax = 254;
inline constexpr std::uint8_t kDurationUnknown = 255;
inline constexpr std::uint16_t kTimeOverflow = 512;

enum class ReadStatus { kOk, kTruncated, kMalformed };

constexpr std::uint8_t Control(std::uint8_t kind, std::uint32_t fields,
                               bool wide = false) noexcept {
  return static_cast<std::uint8_t>(kind | (fields << 2) | (wide ? kWide : 0));
}
constexpr std::uint32_t Fields(std::uint8_t control) noexcept {
  return (control >> 2) & kAllFields;
}
constexpr bool ValidControl(std::uint8_t control) noexcept {
  constexpr auto allowed =
      static_cast<std::uint8_t>(3 | (kAllFields << 2) | kWide);
  auto const kind = control & 3;
  return (control & ~allowed) == 0 && kind <= kMetricBlock &&
         (kind != kMetricBlock || Fields(control) == 0);
}

template <typename Codec>
constexpr std::uint64_t Limit() noexcept {
  if constexpr (std::is_same_v<Codec, TagCodec>)
    return kMaxTag;
  else if constexpr (std::is_same_v<Codec, StreamCodec>)
    return kMaxStream;
  else if constexpr (std::is_same_v<Codec, TimeCodec>)
    return 511;
  else if constexpr (std::is_same_v<Codec, DurationCodec>)
    return kDurationMax;
  else
    return static_cast<std::uint64_t>(Codec::kUpper);
}

// Numeric's runtime range checks are asserts. Check BEFORE construction, also
// in release builds, and serialize to fixed scratch before touching the output.
template <typename Codec>
std::size_t Size(std::uint64_t value) noexcept {
  if (value > Limit<Codec>()) return 0;
  std::array<std::uint8_t, Codec::kMaxWireBytes> scratch{};
  return Codec{value}.Serialize(scratch.data());
}
template <typename Codec>
bool Write(std::uint64_t value, std::span<std::uint8_t> out,
           std::size_t& cursor) noexcept {
  if (value > Limit<Codec>() || cursor > out.size()) return false;
  std::array<std::uint8_t, Codec::kMaxWireBytes> scratch{};
  auto const count = Codec{value}.Serialize(scratch.data());
  if (count > out.size() - cursor) return false;
  std::memcpy(out.data() + cursor, scratch.data(), count);
  cursor += count;
  return true;
}
template <typename Codec>
ReadStatus Read(std::span<std::uint8_t const> input, std::size_t& cursor,
                std::uint64_t& value) noexcept {
  if (cursor > input.size()) return ReadStatus::kMalformed;
  auto const bytes = input.subspan(cursor);
  auto const count = Codec::WireBytesNeeded(bytes.data(), bytes.size());
  if (count == 0) return ReadStatus::kTruncated;
  Codec decoded;
  if (decoded.Deserialize(bytes.data(), count) != count)
    return ReadStatus::kMalformed;
  auto const number = static_cast<std::uint64_t>(decoded.value_);
  if (number > Limit<Codec>()) return ReadStatus::kMalformed;
  std::array<std::uint8_t, Codec::kMaxWireBytes> canonical{};
  auto const encoded = decoded.Serialize(canonical.data());
  if (encoded != count ||
      !std::equal(canonical.begin(), canonical.begin() + count, bytes.begin()))
    return ReadStatus::kMalformed;
  cursor += count;
  value = number;
  return ReadStatus::kOk;
}

// Size includes its own variable-width prefix. Choose the smallest stable
// width: crossing 250/1500 can add more than one byte to the total block size.
inline std::size_t BlockWireSize(std::size_t body_size) noexcept {
  for (std::size_t width : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
    if (body_size > BlockSizeCodec::kUpper - width) return 0;
    auto const total = body_size + width;
    if (Size<BlockSizeCodec>(total) == width) return total;
  }
  return 0;
}

// Body starts at kBlockPrefixReserve. Only the prefix is written here, so
// outstanding duration offsets remain valid across prefix-width transitions.
// Invalid/oversize input leaves storage unchanged.
inline std::span<std::uint8_t const> FinishBlock(
    std::span<std::uint8_t> storage, std::size_t end,
    std::size_t capacity = BlockSizeCodec::kUpper) noexcept {
  if (end < kBlockPrefixReserve || end > storage.size()) return {};
  auto const body_size = end - kBlockPrefixReserve;
  auto const total = BlockWireSize(body_size);
  if (!total || total > capacity) return {};
  auto const start = kBlockPrefixReserve - (total - body_size);
  auto cursor = start;
  if (!Write<BlockSizeCodec>(total, storage, cursor)) return {};
  return storage.subspan(start, total);
}

inline bool WriteU64(std::uint64_t value, std::span<std::uint8_t> out,
                     std::size_t& cursor) noexcept {
  if (cursor > out.size() || out.size() - cursor < 8) return false;
  Store64(out.data() + cursor, value);
  cursor += 8;
  return true;
}
inline ReadStatus ReadU64(std::span<std::uint8_t const> input,
                          std::size_t& cursor, std::uint64_t& value) noexcept {
  if (cursor > input.size()) return ReadStatus::kMalformed;
  if (input.size() - cursor < 8) return ReadStatus::kTruncated;
  value = Load64(input.data() + cursor);
  cursor += 8;
  return ReadStatus::kOk;
}

namespace detail {
static_assert(RuntimeSeconds::kScaleExp == -25);
inline std::uint64_t Microseconds(RuntimeSeconds value,
                                  bool nearest = false) noexcept {
  auto const half = nearest ? (std::uint64_t{1} << 24) : 0;
  return (static_cast<std::uint64_t>(value.RawValue()) * 1'000'000 + half) >>
         25;
}
template <typename Exp, std::size_t Count, bool Nearest = false>
std::array<std::uint64_t, Count> Values() noexcept {
  std::array<std::uint64_t, Count> result{};
  for (std::size_t i = 1; i < Count; ++i) {
    result[i] = Microseconds(Exp::Code(typename Exp::wire_type{i}).ToRuntime(),
                             Nearest);
    // The decoder's table must remain monotone even if a numeric policy
    // changes.
    result[i] = std::max(result[i], result[i - 1]);
  }
  result[1] = kDurationMinUs;
  result.back() = kDurationMaxUs;
  return result;
}
}  // namespace detail

// Published in the debug map too. Python uses these exact integer tables rather
// than recreating numeric's finite-precision log2/exp2 with host floating math.
inline std::array<std::uint64_t, 512> const& TimeValues() noexcept {
  static auto const values = detail::Values<TimeExp, 512>();
  return values;
}
inline std::array<std::uint64_t, 254> const& DurationValues() noexcept {
  static auto const values = detail::Values<DurationExp, 254, true>();
  return values;
}
inline std::uint64_t TimeValue(std::uint16_t code) noexcept {
  return code <= 511 ? TimeValues()[code] : 0;
}
inline std::uint16_t EncodeTime(std::uint64_t delta_us) noexcept {
  if (delta_us > kMaxTimeDeltaUs) return kTimeOverflow;
  auto const& values = TimeValues();
  auto const next = std::upper_bound(values.begin(), values.end(), delta_us);
  return static_cast<std::uint16_t>(next - values.begin() - 1);
}

inline std::uint8_t EncodeDuration(std::uint64_t duration_us) noexcept {
  if (duration_us < kDurationMinUs) return kDurationBelowMin;
  if (duration_us > kDurationMaxUs) return kDurationAboveMax;
  if (duration_us == kDurationMinUs) return kDurationMin;
  if (duration_us == kDurationMaxUs) return kDurationMax;
  // Pick the nearest value of the actual numeric decoder. Applying an
  // approximate inverse and then truncating to integer microseconds would add
  // avoidable bias, especially for durations of only a few microseconds.
  auto const& values = DurationValues();
  auto next = std::lower_bound(values.begin() + 1, values.end(), duration_us);
  if (next != values.begin() + 1 &&
      duration_us - *(next - 1) <= *next - duration_us)
    --next;
  return static_cast<std::uint8_t>(next - values.begin());
}
inline DurationValue DecodeDuration(std::uint8_t code) noexcept {
  if (code == kDurationBelowMin) return {DurationState::kBelowMin, 0};
  if (code == kDurationAboveMax) return {DurationState::kAboveMax, 0};
  if (code == kDurationUnknown) return {DurationState::kUnknown, 0};
  return {DurationState::kValue, DurationValues()[code]};
}

static_assert(TagCodec::kMaxWireBytes == 4);
static_assert(StreamCodec::kMaxWireBytes == 4);
static_assert(BlobCodec::kBaseBytes == 1 && BlobCodec::kMaxWireBytes == 4);
static_assert(BlobCodec::kUpper == 1'967'580);
}  // namespace ae::tele::raw::wire
#endif  // AETHER_TELE_RAW_WIRE_H_
