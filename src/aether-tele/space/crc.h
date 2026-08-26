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

#ifndef AETHER_TELE_SPACE_CRC_H_
#define AETHER_TELE_SPACE_CRC_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aether-miscpp/crc.h"

namespace ae::tele::space {

// IEEE CRC-32 over an explicit little-endian byte stream. Not ABI-dependent.
struct Crc32Acc {
  crc32::result_t state{};

  constexpr void FeedByte(std::uint8_t byte) {
    state.value =
        (state.value >> 8) ^
        crc32::details::table[static_cast<std::uint8_t>(state.value) ^ byte];
  }

  constexpr void FeedU8(std::uint8_t value) { FeedByte(value); }

  constexpr void FeedU32(std::uint32_t value) {
    FeedByte(static_cast<std::uint8_t>(value));
    FeedByte(static_cast<std::uint8_t>(value >> 8));
    FeedByte(static_cast<std::uint8_t>(value >> 16));
    FeedByte(static_cast<std::uint8_t>(value >> 24));
  }

  constexpr void FeedU64(std::uint64_t value) {
    FeedU32(static_cast<std::uint32_t>(value));
    FeedU32(static_cast<std::uint32_t>(value >> 32));
  }

  constexpr void FeedBytes(std::uint8_t const* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      FeedByte(data[i]);
    }
  }

  constexpr void FeedString(std::string_view text) {
    for (char c : text) {
      FeedByte(static_cast<std::uint8_t>(c));
    }
    FeedByte(0);
  }

  constexpr std::uint32_t Finish() const {
    return state.value ^ crc32::details::XOR_VALUE;
  }
};

constexpr void WriteU32LE(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8);
  out[2] = static_cast<std::uint8_t>(value >> 16);
  out[3] = static_cast<std::uint8_t>(value >> 24);
}

constexpr std::uint32_t ReadU32LE(std::uint8_t const* in) {
  return static_cast<std::uint32_t>(in[0]) |
         (static_cast<std::uint32_t>(in[1]) << 8) |
         (static_cast<std::uint32_t>(in[2]) << 16) |
         (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_CRC_H_
