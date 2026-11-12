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
#ifndef AETHER_TELE_TESTS_RAW_COMPACT_NUMERIC_H_
#define AETHER_TELE_TESTS_RAW_COMPACT_NUMERIC_H_

#include <unity.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "aether-tele/raw/wire.h"

namespace compact_numeric_tests {
namespace raw = ae::tele::raw;
namespace wire = raw::wire;

template <typename Codec>
void CheckRoundTrip(std::uint64_t number, std::size_t expected_size) {
  std::array<std::uint8_t, 32> bytes;
  bytes.fill(0xa5);
  auto const untouched = bytes;
  std::size_t cursor = 3;
  TEST_ASSERT_EQUAL_UINT(expected_size, wire::Size<Codec>(number));
  TEST_ASSERT_TRUE(wire::Write<Codec>(number, bytes, cursor));
  TEST_ASSERT_EQUAL_UINT(3 + expected_size, cursor);
  TEST_ASSERT_EQUAL_MEMORY(untouched.data(), bytes.data(), 3);
  TEST_ASSERT_EQUAL_MEMORY(untouched.data() + cursor, bytes.data() + cursor,
                           bytes.size() - cursor);

  std::size_t read = 3;
  std::uint64_t decoded = 123;
  TEST_ASSERT_TRUE(wire::Read<Codec>(bytes, read, decoded) ==
                   wire::ReadStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(cursor, read);
  TEST_ASSERT_EQUAL_UINT64(number, decoded);

  for (std::size_t available = 0; available < expected_size; ++available) {
    read = 3;
    decoded = 123;
    TEST_ASSERT_TRUE(
        wire::Read<Codec>(
            std::span<std::uint8_t const>{bytes}.first(3 + available), read,
            decoded) == wire::ReadStatus::kTruncated);
    TEST_ASSERT_EQUAL_UINT(3, read);
    TEST_ASSERT_EQUAL_UINT64(123, decoded);

    auto output = untouched;
    std::size_t write = 3;
    TEST_ASSERT_FALSE(wire::Write<Codec>(
        number, std::span<std::uint8_t>{output}.first(3 + available), write));
    TEST_ASSERT_EQUAL_UINT(3, write);
    TEST_ASSERT_EQUAL_MEMORY(untouched.data(), output.data(), output.size());
  }
}

template <typename Codec, std::size_t N>
void CheckCases(
    std::array<std::pair<std::uint64_t, std::size_t>, N> const& cases) {
  for (auto const& [number, size] : cases) CheckRoundTrip<Codec>(number, size);
}

inline void test_codec_tier_boundaries_and_atomic_partial_io() {
  using Case = std::pair<std::uint64_t, std::size_t>;
  CheckCases<wire::TagCodec>(std::array{Case{0, 1}, Case{1, 1}, Case{239, 1},
                                        Case{240, 2}, Case{4095, 2},
                                        Case{4096, 4}, Case{65535, 4}});
  CheckCases<wire::StreamCodec>(std::array{Case{0, 1}, Case{15, 1}, Case{16, 2},
                                           Case{1023, 2}, Case{1024, 4},
                                           Case{65535, 4}});
  CheckCases<wire::SequenceCodec>(std::array{
      Case{0, 2}, Case{32767, 2}, Case{32768, 4}, Case{2'000'000'000, 4},
      Case{2'000'000'001, 8}, Case{wire::Limit<wire::SequenceCodec>(), 8}});
  CheckCases<wire::SumCodec>(std::array{
      Case{0, 2}, Case{32767, 2}, Case{32768, 4}, Case{2'000'000'000, 4},
      Case{2'000'000'001, 8}, Case{wire::Limit<wire::SumCodec>(), 8}});
  CheckCases<wire::CountCodec>(
      std::array{Case{0, 1}, Case{247, 1}, Case{248, 2}, Case{2047, 2},
                 Case{2048, 4}, Case{16'000'000, 4}, Case{16'000'001, 8},
                 Case{wire::Limit<wire::CountCodec>(), 8}});
  CheckCases<wire::BlobCodec>(std::array{
      Case{0, 1}, Case{250, 1}, Case{251, 2}, Case{1500, 2}, Case{1501, 4},
      Case{1'967'580, 4}, Case{wire::Limit<wire::BlobCodec>(), 4}});
  CheckCases<wire::TimeCodec>(
      std::array{Case{0, 1}, Case{250, 1}, Case{251, 2}, Case{511, 2}});
  CheckCases<wire::DurationCodec>(
      std::array{Case{0, 1}, Case{1, 1}, Case{252, 1}, Case{253, 1}});
}

template <typename Codec>
void CheckOutOfRange() {
  auto const invalid = wire::Limit<Codec>() + 1;
  std::array<std::uint8_t, 32> bytes;
  bytes.fill(0xa5);
  auto const untouched = bytes;
  std::size_t cursor = 3;
  TEST_ASSERT_EQUAL_UINT(0, wire::Size<Codec>(invalid));
  TEST_ASSERT_FALSE(wire::Write<Codec>(invalid, bytes, cursor));
  TEST_ASSERT_EQUAL_UINT(3, cursor);
  TEST_ASSERT_EQUAL_MEMORY(untouched.data(), bytes.data(), bytes.size());

  std::uint64_t value = 123;
  cursor = bytes.size() + 1;
  TEST_ASSERT_TRUE(wire::Read<Codec>(bytes, cursor, value) ==
                   wire::ReadStatus::kMalformed);
  TEST_ASSERT_EQUAL_UINT(bytes.size() + 1, cursor);
  TEST_ASSERT_EQUAL_UINT64(123, value);
  TEST_ASSERT_FALSE(wire::Write<Codec>(0, bytes, cursor));
  TEST_ASSERT_EQUAL_UINT(bytes.size() + 1, cursor);
  TEST_ASSERT_EQUAL_MEMORY(untouched.data(), bytes.data(), bytes.size());

  cursor = 0;
  TEST_ASSERT_TRUE(wire::Read<Codec>({}, cursor, value) ==
                   wire::ReadStatus::kTruncated);
  TEST_ASSERT_EQUAL_UINT(0, cursor);
  TEST_ASSERT_EQUAL_UINT64(123, value);
}

template <typename Codec>
void CheckMalformedLogicalValue(std::uint64_t invalid) {
  // The numeric type can represent this value, but the telemetry format cannot.
  std::array<std::uint8_t, Codec::kMaxWireBytes> bytes{};
  auto const size = Codec{invalid}.Serialize(bytes.data());
  std::size_t cursor = 0;
  std::uint64_t value = 123;
  TEST_ASSERT_TRUE(wire::Read<Codec>(std::span{bytes}.first(size), cursor,
                                     value) == wire::ReadStatus::kMalformed);
  TEST_ASSERT_EQUAL_UINT(0, cursor);
  TEST_ASSERT_EQUAL_UINT64(123, value);
}

inline void test_codec_rejects_overflow_and_invalid_logical_values() {
  CheckOutOfRange<wire::TagCodec>();
  CheckOutOfRange<wire::StreamCodec>();
  CheckOutOfRange<wire::SequenceCodec>();
  CheckOutOfRange<wire::CountCodec>();
  CheckOutOfRange<wire::SumCodec>();
  CheckOutOfRange<wire::BlobCodec>();
  CheckOutOfRange<wire::TimeCodec>();
  CheckOutOfRange<wire::DurationCodec>();
  CheckMalformedLogicalValue<wire::TagCodec>(65536);
  CheckMalformedLogicalValue<wire::StreamCodec>(65536);
  CheckMalformedLogicalValue<wire::TimeCodec>(512);
  CheckMalformedLogicalValue<wire::DurationCodec>(254);
}

inline void test_wide_u64_keeps_full_range_and_partial_io_atomic() {
  for (auto number : {std::uint64_t{0}, std::uint64_t{1},
                      std::numeric_limits<std::uint64_t>::max()}) {
    std::array<std::uint8_t, 16> bytes;
    bytes.fill(0xa5);
    auto const untouched = bytes;
    std::size_t cursor = 3;
    TEST_ASSERT_TRUE(wire::WriteU64(number, bytes, cursor));
    TEST_ASSERT_EQUAL_UINT(11, cursor);
    std::size_t read = 3;
    std::uint64_t decoded = 123;
    TEST_ASSERT_TRUE(wire::ReadU64(bytes, read, decoded) ==
                     wire::ReadStatus::kOk);
    TEST_ASSERT_EQUAL_UINT64(number, decoded);
    TEST_ASSERT_EQUAL_UINT(11, read);
    for (std::size_t available = 0; available < 8; ++available) {
      read = 3;
      decoded = 123;
      TEST_ASSERT_TRUE(
          wire::ReadU64(
              std::span<std::uint8_t const>{bytes}.first(3 + available), read,
              decoded) == wire::ReadStatus::kTruncated);
      TEST_ASSERT_EQUAL_UINT(3, read);
      TEST_ASSERT_EQUAL_UINT64(123, decoded);
      auto output = untouched;
      cursor = 3;
      TEST_ASSERT_FALSE(wire::WriteU64(
          number, std::span<std::uint8_t>{output}.first(3 + available),
          cursor));
      TEST_ASSERT_EQUAL_UINT(3, cursor);
      TEST_ASSERT_EQUAL_MEMORY(untouched.data(), output.data(), output.size());
    }
  }
}

inline void test_time_codes_floor_without_overshoot() {
  auto const& values = wire::TimeValues();
  TEST_ASSERT_EQUAL_UINT64(0, values.front());
  TEST_ASSERT_EQUAL_UINT64(1, values[1]);
  TEST_ASSERT_EQUAL_UINT64(wire::kMaxTimeDeltaUs, values.back());
  for (std::size_t code = 0; code < values.size(); ++code) {
    TEST_ASSERT_EQUAL_UINT64(values[code],
                             wire::TimeValue(static_cast<std::uint16_t>(code)));
    TEST_ASSERT_EQUAL_UINT(code <= 250 ? 1 : 2,
                           wire::Size<wire::TimeCodec>(code));
    auto const encoded = wire::EncodeTime(values[code]);
    TEST_ASSERT_EQUAL_UINT64(values[code], wire::TimeValue(encoded));
    if (code == 0) continue;
    TEST_ASSERT_TRUE(values[code] >= values[code - 1]);
    if (values[code] != values[code - 1]) {
      auto const before = wire::EncodeTime(values[code] - 1);
      TEST_ASSERT_EQUAL_UINT64(values[code - 1], wire::TimeValue(before));
    }
  }
  TEST_ASSERT_EQUAL_UINT(1,
                         wire::Size<wire::TimeCodec>(wire::EncodeTime(6535)));
  TEST_ASSERT_EQUAL_UINT(2,
                         wire::Size<wire::TimeCodec>(wire::EncodeTime(6536)));
  TEST_ASSERT_EQUAL_UINT(wire::kTimeOverflow,
                         wire::EncodeTime(wire::kMaxTimeDeltaUs + 1));
  TEST_ASSERT_EQUAL_UINT(
      wire::kTimeOverflow,
      wire::EncodeTime(std::numeric_limits<std::uint64_t>::max()));
}

inline std::uint64_t Difference(std::uint64_t a, std::uint64_t b) {
  return a > b ? a - b : b - a;
}
inline void CheckNearestDuration(std::uint64_t microseconds,
                                 std::uint64_t lower, std::uint64_t upper) {
  auto const code = wire::EncodeDuration(microseconds);
  TEST_ASSERT_TRUE(code >= 1 && code <= 253);
  auto const decoded = wire::DecodeDuration(code);
  TEST_ASSERT_TRUE(decoded.state == raw::DurationState::kValue);
  auto const error = Difference(decoded.microseconds, microseconds);
  TEST_ASSERT_EQUAL_UINT64(std::min(Difference(lower, microseconds),
                                    Difference(upper, microseconds)),
                           error);
  // The worst integer-us relative error is 1/18 at 18 us (17/19 tie).
  TEST_ASSERT_TRUE(error * 18 <= microseconds);
}

inline void test_duration_nearest_values_and_explicit_range_states() {
  TEST_ASSERT_EQUAL_UINT8(0, wire::EncodeDuration(0));
  TEST_ASSERT_EQUAL_UINT8(1, wire::EncodeDuration(1));
  TEST_ASSERT_EQUAL_UINT8(253, wire::EncodeDuration(wire::kDurationMaxUs));
  TEST_ASSERT_EQUAL_UINT8(254, wire::EncodeDuration(wire::kDurationMaxUs + 1));
  TEST_ASSERT_EQUAL_UINT8(
      254, wire::EncodeDuration(std::numeric_limits<std::uint64_t>::max()));
  TEST_ASSERT_TRUE(wire::DecodeDuration(0).state ==
                   raw::DurationState::kBelowMin);
  TEST_ASSERT_TRUE(wire::DecodeDuration(254).state ==
                   raw::DurationState::kAboveMax);
  TEST_ASSERT_TRUE(wire::DecodeDuration(255).state ==
                   raw::DurationState::kUnknown);
  TEST_ASSERT_FALSE(wire::DecodeDuration(0).known());
  TEST_ASSERT_FALSE(wire::DecodeDuration(254).known());
  TEST_ASSERT_FALSE(wire::DecodeDuration(255).known());
  TEST_ASSERT_EQUAL_UINT64(1, wire::DecodeDuration(1).microseconds);
  TEST_ASSERT_EQUAL_UINT64(wire::kDurationMaxUs,
                           wire::DecodeDuration(253).microseconds);
  TEST_ASSERT_EQUAL_UINT64(
      3, wire::DecodeDuration(wire::EncodeDuration(3)).microseconds);

  auto const& values = wire::DurationValues();
  for (std::size_t code = 1; code < values.size(); ++code) {
    auto const value = wire::DecodeDuration(static_cast<std::uint8_t>(code));
    TEST_ASSERT_TRUE(value.known());
    TEST_ASSERT_EQUAL_UINT64(values[code], value.microseconds);
    if (code == 1) continue;
    auto const lower = values[code - 1];
    auto const upper = values[code];
    TEST_ASSERT_TRUE(lower <= upper);
    CheckNearestDuration(upper, upper, upper);
    if (lower == upper) continue;
    // Relative error is largest next to each nearest-code decision boundary;
    // these two integers cover every integer-us duration without a 60M loop.
    auto const midpoint = (lower + upper) / 2;
    CheckNearestDuration(midpoint, lower, upper);
    CheckNearestDuration(midpoint + 1, lower, upper);
  }
}

inline void Run() {
  RUN_TEST(test_codec_tier_boundaries_and_atomic_partial_io);
  RUN_TEST(test_codec_rejects_overflow_and_invalid_logical_values);
  RUN_TEST(test_wide_u64_keeps_full_range_and_partial_io_atomic);
  RUN_TEST(test_time_codes_floor_without_overshoot);
  RUN_TEST(test_duration_nearest_values_and_explicit_range_states);
}
}  // namespace compact_numeric_tests
#endif  // AETHER_TELE_TESTS_RAW_COMPACT_NUMERIC_H_
