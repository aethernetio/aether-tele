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

#ifndef AETHER_TELE_SPACE_CODEC_H_
#define AETHER_TELE_SPACE_CODEC_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ae-numeric/exponential.h"
#include "ae-numeric/exponential_math_policy.h"
#include "ae-numeric/fixed_point.h"
#include "ae-numeric/numeric_traits.h"
#include "ae-numeric/wire_io.h"
#include "aether-tele/space/crc.h"

namespace ae::tele::space {

template <typename T>
struct DecodeResult {
  T value{};
  std::size_t bytes_read{};
};

template <typename C>
concept NumericCodec = requires(typename C::value_type v, std::uint8_t* out,
                                std::uint8_t const* in, std::size_t n) {
  typename C::value_type;
  requires std::unsigned_integral<typename C::value_type>;
  { C::MaxEncodedSize() } -> std::convertible_to<std::size_t>;
  { C::kSchemaSignature } -> std::convertible_to<std::uint32_t>;
  { C::Encode(v, out) } -> std::same_as<std::size_t>;
  { C::Decode(in, n) } -> std::same_as<DecodeResult<typename C::value_type>>;
};

// Packed integer codec over ae-numeric TieredInt. 1/2/4/8 byte tiers — there
// is no 3-byte tier in the existing packed integer type.
template <typename Packed>
struct PackedIntCodec {
  using value_type = std::uint64_t;
  using packed_type = Packed;

  static constexpr std::size_t kMaxEncodedSize = Packed::kMaxWireBytes;
  static constexpr std::size_t MaxEncodedSize() { return kMaxEncodedSize; }
  static constexpr std::uint32_t kSchemaSignature = []() constexpr {
    Crc32Acc crc;
    crc.FeedString("PackedIntCodec");
    crc.FeedU32(static_cast<std::uint32_t>(Packed::kNumTiers));
    crc.FeedU32(static_cast<std::uint32_t>(Packed::kMaxWireBytes));
    crc.FeedU32(static_cast<std::uint32_t>(Packed::kWireTier0));
    return crc.Finish();
  }();

  static std::size_t Encode(value_type value, std::uint8_t* out) {
    Packed packed{value};
    return ae::Serialize(packed, out);
  }

  static DecodeResult<value_type> Decode(std::uint8_t const* in,
                                         std::size_t len) {
    if (in == nullptr || len == 0) {
      return {};
    }
    auto const decoded = ae::Deserialize<Packed>(in, len);
    if (decoded.bytes_read == 0) {
      return {};
    }
    return DecodeResult<value_type>{
        .value = static_cast<value_type>(
            static_cast<typename Packed::ValueType>(decoded.value)),
        .bytes_read = decoded.bytes_read,
    };
  }
};

// Default ae-numeric work-type picking only reaches Max=65536, which cannot
// represent a 4e9-tick boundary. This policy keeps the library Exponential
// mapping and substitutes a FixedPoint work type whose declared max is the
// boundary magnitude.
template <typename RuntimeT, auto MinMagnitude, auto BoundaryMagnitude,
          auto BoundaryCode, typename WireValue, bool IsSigned>
struct WideExponentialMathPolicy {
  using Default = exponential_internal::ExponentialMathPolicy<
      RuntimeT, MinMagnitude, BoundaryMagnitude, BoundaryCode, WireValue,
      IsSigned>;
  using log_type = typename Default::log_type;
  using abs_log_type = typename Default::abs_log_type;
  using mant_type = typename Default::mant_type;
  static constexpr int kLogIterations = Default::kLogIterations;
  static constexpr int kExp2FractionBits = Default::kExp2FractionBits;
  using work_type =
      FixedPoint<std::uint32_t, static_cast<double>(BoundaryMagnitude)>;
  using mul_intermediate_type =
      typename exponential_internal::MulIntermediate<
          typename work_type::rep_value_type>::type;
};

template <typename Runtime>
constexpr std::uint64_t RuntimeToU64(Runtime const& runtime) {
  if constexpr (std::is_integral_v<Runtime>) {
    return static_cast<std::uint64_t>(runtime);
  } else if constexpr (requires {
                         Runtime::kScaleExp;
                         runtime.RawValue();
                       }) {
    auto const raw = static_cast<std::int64_t>(runtime.RawValue());
    int const scale = Runtime::kScaleExp;
    std::int64_t logical = raw;
    if (scale > 0) {
      logical = raw * (static_cast<std::int64_t>(1) << scale);
    } else if (scale < 0) {
      auto const den = static_cast<std::int64_t>(1) << (-scale);
      logical = (raw + (raw >= 0 ? den / 2 : -(den / 2))) / den;
    }
    if (logical < 0) {
      return 0;
    }
    return static_cast<std::uint64_t>(logical);
  } else {
    return static_cast<std::uint64_t>(runtime);
  }
}

// Exponential magnitude codec over ae::Exponential. Storage ticks stay uint64;
// the Exponential runtime type may be integer or FixedPoint. Wire codes are
// the existing exponential type — this is not a second packed-integer codec.
template <typename Exp>
struct ExponentialCodec {
  using value_type = std::uint64_t;
  using exponential_type = Exp;

  static constexpr std::size_t kMaxEncodedSize =
      ae::wire_traits<typename Exp::wire_type>::kMaxWireBytes;
  static constexpr std::size_t MaxEncodedSize() { return kMaxEncodedSize; }
  static constexpr std::uint32_t kSchemaSignature = []() constexpr {
    Crc32Acc crc;
    crc.FeedString("ExponentialCodec");
    crc.FeedU64(static_cast<std::uint64_t>(Exp::kMinMagnitude));
    crc.FeedU64(static_cast<std::uint64_t>(Exp::kBoundaryMagnitude));
    crc.FeedU32(static_cast<std::uint32_t>(Exp::kBoundaryCode));
    crc.FeedU32(static_cast<std::uint32_t>(sizeof(typename Exp::runtime_type)));
    if constexpr (requires { Exp::runtime_type::kFractionBits; }) {
      crc.FeedU32(static_cast<std::uint32_t>(Exp::runtime_type::kFractionBits));
      crc.FeedU32(static_cast<std::uint32_t>(Exp::runtime_type::kScaleExp));
    }
    return crc.Finish();
  }();

  static std::size_t Encode(value_type value, std::uint8_t* out) {
    auto const bound = static_cast<value_type>(Exp::kBoundaryMagnitude);
    if (value > bound) {
      value = bound;
    }
    auto const encoded =
        Exp::FromRuntimeInteger(static_cast<std::int64_t>(value));
    return ae::Serialize(encoded.WireCode(), out);
  }

  static DecodeResult<value_type> Decode(std::uint8_t const* in,
                                         std::size_t len) {
    if (in == nullptr || len == 0) {
      return {};
    }
    auto const wire =
        ae::Deserialize<typename Exp::wire_type>(in, len);
    if (wire.bytes_read == 0) {
      return {};
    }
    auto const decoded = Exp::FromCode(wire.value);
    return DecodeResult<value_type>{
        .value = RuntimeToU64(decoded.Value()),
        .bytes_read = wire.bytes_read,
    };
  }
};

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_CODEC_H_
