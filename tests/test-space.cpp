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

#include "tele_test_common.h"

#include <unity.h>

#include <array>
#include <cstdint>
#include <type_traits>

#include "aether-tele/packed_int.h"
#include "aether-tele/space.h"

namespace ae::tele::test_space {
using ae::tele::Level;
using ae::tele::PackedU64;
using namespace ae::tele::space;

namespace space_a {

inline constexpr std::uint32_t kIo = 1;
inline constexpr std::uint32_t kInternal = 99;

#define SPACE_A_TAGS(X)                                                     \
  X(Ping, 42, Log, None, true, kIo, Info)                                   \
  X(AuthFail, 1, Log, PackedU32, true, kIo, Error)                          \
  X(Pulse, 4, Log, None, false, kIo, Info)                                  \
  X(Dropped, 3000, Log, None, true, kInternal, Debug)                       \
  X(Scope, 90, Duration, None, true, kIo, Info)

SPACE_A_TAGS(AE_SPACE_POINT)

struct Config {
  using CountType = std::uint32_t;
  using TimeType = std::uint64_t;
  using CountCodec = PackedIntCodec<PackedU64>;
  using TimeCodec = PackedIntCodec<PackedU64>;
  using DeltaCodec = PackedIntCodec<PackedU64>;
  using IndexCodec = PackedIntCodec<PackedU64>;
  using Clock = ManualClock<TimeType, Config>;
  using UnixClock = ManualUnixClock<Config>;
  static constexpr std::size_t kMaxStreams = 8;
  static constexpr std::size_t kStreamCapacity = 4096;
  static constexpr std::uint32_t kTicksPerSecond = 1'000'000u;

  static constexpr auto kTags = std::array{SPACE_A_TAGS(AE_SPACE_POINT_REF)};
  static_assert(UniqueDirectIndices(kTags));

  static constexpr std::uint32_t kSchemaChecksum = ComputeSchemaChecksum(
      kSpaceFormatVersion, kMetricsFormatVersion, CountCodec::kSchemaSignature,
      TimeCodec::kSchemaSignature, DeltaCodec::kSchemaSignature,
      IndexCodec::kSchemaSignature, kTicksPerSecond, kTags);

#if defined(AE_TELE_SPACE_HOST)
  static constexpr auto kHostTags = std::array{
      HostTag{42, "Ping", "AlphaIo", Level::kInfo},
      HostTag{1, "AuthFail", "AlphaIo", Level::kError},
      HostTag{4, "Pulse", "AlphaIo", Level::kInfo},
      HostTag{3000, "Dropped", "AlphaInternal", Level::kDebug},
      HostTag{90, "Scope", "AlphaIo", Level::kInfo},
  };
#endif

  template <Level::underlined_t, std::uint32_t ModuleId>
  static consteval SpaceTeleConfig GetTeleConfig() {
    if constexpr (ModuleId == kInternal) {
      return SpaceTeleConfig{false, false, false};
    }
    return SpaceTeleConfig{};
  }
};

using Storage = TeleStorage<Config>;
using Stream = ::ae::tele::space::Stream<Config>;
using Clock = Config::Clock;
}  // namespace space_a

namespace space_b {

inline constexpr std::uint32_t kApp = 7;

#define SPACE_B_TAGS(X) \
  X(Frame, 42, Log, None, true, kApp, Info) X(Fatal, 1, Log, PackedU32, true, kApp, Error)

SPACE_B_TAGS(AE_SPACE_POINT)

struct Config {
  using CountType = std::uint64_t;
  using TimeType = std::uint64_t;
  using CountCodec = PackedIntCodec<PackedU64>;
  using TimeCodec = PackedIntCodec<PackedU64>;
  using DeltaCodec = PackedIntCodec<PackedU64>;
  using IndexCodec = PackedIntCodec<PackedU64>;
  using Clock = ManualClock<TimeType, Config>;
  using UnixClock = ManualUnixClock<Config>;
  static constexpr std::size_t kMaxStreams = 8;
  static constexpr std::size_t kStreamCapacity = 4096;
  static constexpr std::uint32_t kTicksPerSecond = 1'000'000u;

  static constexpr auto kTags = std::array{SPACE_B_TAGS(AE_SPACE_POINT_REF)};
  static_assert(UniqueDirectIndices(kTags));

  static constexpr std::uint32_t kSchemaChecksum = ComputeSchemaChecksum(
      kSpaceFormatVersion, kMetricsFormatVersion, CountCodec::kSchemaSignature,
      TimeCodec::kSchemaSignature, DeltaCodec::kSchemaSignature,
      IndexCodec::kSchemaSignature, kTicksPerSecond, kTags);

#if defined(AE_TELE_SPACE_HOST)
  static constexpr auto kHostTags = std::array{
      HostTag{42, "Frame", "BetaApp", Level::kInfo},
      HostTag{1, "Fatal", "BetaApp", Level::kError},
  };
#endif

  template <Level::underlined_t, std::uint32_t>
  static consteval SpaceTeleConfig GetTeleConfig() {
    return SpaceTeleConfig{};
  }
};

using Storage = TeleStorage<Config>;
using Stream = ::ae::tele::space::Stream<Config>;
using Clock = Config::Clock;
}  // namespace space_b

#define A_TELE_INFO(STREAM, TAG, ...) \
  AE_SPACE_TELE_INFO(STREAM, TAG, __VA_ARGS__)
#define A_TELE_ERROR(STREAM, TAG, ...) \
  AE_SPACE_TELE_ERROR(STREAM, TAG, __VA_ARGS__)
#define A_TELE_DEBUG(STREAM, TAG, ...) \
  AE_SPACE_TELE_DEBUG(STREAM, TAG, __VA_ARGS__)
#define B_TELE_INFO(STREAM, TAG, ...) \
  AE_SPACE_TELE_INFO(STREAM, TAG, __VA_ARGS__)
#define B_TELE_ERROR(STREAM, TAG, ...) \
  AE_SPACE_TELE_ERROR(STREAM, TAG, __VA_ARGS__)

void EmitAPing(space_a::Stream& stream) { A_TELE_INFO(stream, space_a::kPing); }
void EmitAPulse(space_a::Stream& stream) { A_TELE_INFO(stream, space_a::kPulse); }
void EmitAAuth(space_a::Stream& stream) {
  A_TELE_ERROR(stream, space_a::kAuthFail, 7u);
}
void EmitADropped(space_a::Stream& stream) {
  A_TELE_DEBUG(stream, space_a::kDropped);
}
void EmitBFrame(space_b::Stream& stream) { B_TELE_INFO(stream, space_b::kFrame); }
void EmitBFatal(space_b::Stream& stream) {
  B_TELE_ERROR(stream, space_b::kFatal, 1u);
}

void test_SpaceChecksumIsUint32CompileTime() {
  static_assert(
      std::is_same_v<std::remove_cv_t<decltype(space_a::Config::kSchemaChecksum)>,
                     std::uint32_t>);
  static_assert(sizeof(space_a::Config::kSchemaChecksum) == 4);
  static_assert(space_a::Config::kSchemaChecksum !=
                space_b::Config::kSchemaChecksum);
  TEST_ASSERT_EQUAL(4, sizeof(space_a::Config::kSchemaChecksum));
}

void test_SpaceIndexChangeChangesChecksum() {
  constexpr auto tags = space_a::Config::kTags;
  constexpr auto altered = [] {
    auto copy = space_a::Config::kTags;
    copy[0].index = copy[0].index + 1;
    return copy;
  }();
  constexpr auto original = ComputeSchemaChecksum(
      kSpaceFormatVersion, kMetricsFormatVersion,
      space_a::Config::CountCodec::kSchemaSignature,
      space_a::Config::TimeCodec::kSchemaSignature,
      space_a::Config::DeltaCodec::kSchemaSignature,
      space_a::Config::IndexCodec::kSchemaSignature,
      space_a::Config::kTicksPerSecond, tags);
  constexpr auto changed = ComputeSchemaChecksum(
      kSpaceFormatVersion, kMetricsFormatVersion,
      space_a::Config::CountCodec::kSchemaSignature,
      space_a::Config::TimeCodec::kSchemaSignature,
      space_a::Config::DeltaCodec::kSchemaSignature,
      space_a::Config::IndexCodec::kSchemaSignature,
      space_a::Config::kTicksPerSecond, altered);
  static_assert(original != changed);
  TEST_ASSERT_NOT_EQUAL(original, changed);
}

void test_TwoSpacesIndependentSameIndex() {
  space_a::Clock::reset(0);
  space_b::Clock::reset(0);
  space_a::Storage storage_a;
  space_b::Storage storage_b;
  space_a::Stream stream_a{storage_a, StreamMarker::kMain};
  space_b::Stream stream_b{storage_b, StreamMarker::kMain};

  space_a::Clock::advance(50);
  EmitAPing(stream_a);
  space_a::Clock::advance(10);
  EmitAPing(stream_a);
  space_b::Clock::advance(999);
  EmitBFrame(stream_b);
  EmitAAuth(stream_a);
  EmitBFatal(stream_b);

  TEST_ASSERT_EQUAL(2, storage_a.TryMetric(space_a::kPing.index)->count);
  TEST_ASSERT_EQUAL(1, storage_b.TryMetric(space_b::kFrame.index)->count);
  TEST_ASSERT_EQUAL(space_a::kPing.index, space_b::kFrame.index);
  TEST_ASSERT_EQUAL(space_a::kAuthFail.index, space_b::kFatal.index);
  TEST_ASSERT_NOT_NULL(storage_a.TryMetric(42));
  TEST_ASSERT_NOT_NULL(storage_b.TryMetric(42));
  TEST_ASSERT_EQUAL(2, storage_a.TryMetric(42)->count);
  TEST_ASSERT_EQUAL(1, storage_b.TryMetric(42)->count);

  storage_a.SetBaseUnix(1'700'000'000u);
  storage_b.SetBaseUnix(1'700'000'001u);
  auto const blob_a = SerializeBlob(storage_a);
  auto const blob_b = SerializeBlob(storage_b);
  auto decoded_a = DecodeBlob<space_a::Config>(blob_a);
  auto decoded_b = DecodeBlob<space_b::Config>(blob_b);
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kOk),
                    static_cast<int>(decoded_a.status));
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kOk),
                    static_cast<int>(decoded_b.status));
  std::uint64_t ping_count = 0;
  for (auto const& metric : decoded_a.data->metrics) {
    if (metric.index == 42) {
      ping_count = metric.count;
    }
  }
  TEST_ASSERT_EQUAL_UINT64(2, ping_count);

  auto const reject_b_as_a = DecodeBlob<space_a::Config>(blob_b);
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kChecksumMismatch),
                    static_cast<int>(reject_b_as_a.status));
}

void test_ExactDeltaAndCountRoundTrip() {
  space_a::Clock::reset(1000);
  space_a::Storage storage;
  space_a::Stream stream{storage, StreamMarker::kMain};
  space_a::Clock::advance(20);
  EmitAPing(stream);
  space_a::Clock::advance(1250);
  EmitAPing(stream);
  auto const blob = SerializeBlob(storage);
  auto decoded = DecodeBlob<space_a::Config>(blob);
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kOk),
                    static_cast<int>(decoded.status));
  TEST_ASSERT_EQUAL(2, decoded.data->events.size());
  TEST_ASSERT_EQUAL_UINT64(20, decoded.data->events[0].delta);
  TEST_ASSERT_EQUAL_UINT64(1250, decoded.data->events[1].delta);
  TEST_ASSERT_EQUAL_UINT64(20, decoded.data->events[0].rel_time);
  TEST_ASSERT_EQUAL_UINT64(1270, decoded.data->events[1].rel_time);
  TEST_ASSERT_EQUAL_UINT64(2, decoded.data->metrics[0].count);
}

void test_BareUntimedRecordCanBeOneByte() {
  space_a::Clock::reset(0);
  space_a::Storage storage;
  space_a::Stream stream{storage, StreamMarker::kMain};
  EmitAPulse(stream);
  TEST_ASSERT_EQUAL(1, storage.log_bytes().size());
  TEST_ASSERT_EQUAL(4, storage.log_bytes()[0]);
}

void test_DisabledModuleCreatesNoData() {
  space_a::Clock::reset(0);
  space_a::Storage storage;
  space_a::Stream stream{storage, StreamMarker::kMain};
  for (int i = 0; i < 16; ++i) {
    EmitADropped(stream);
  }
  TEST_ASSERT_NULL(storage.TryMetric(space_a::kDropped.index));
  TEST_ASSERT_EQUAL(0, storage.log_bytes().size());
  TEST_ASSERT_FALSE(storage.HasAnyMetric());
}

void test_BinaryOmitsPresentationStrings() {
  space_a::Clock::reset(0);
  space_a::Storage storage;
  space_a::Stream stream{storage, StreamMarker::kMain};
  space_a::Clock::advance(5);
  EmitAPing(stream);
  EmitAAuth(stream);
  auto const blob = SerializeBlob(storage);
  TEST_ASSERT_FALSE(BlobContainsText(blob, "Ping"));
  TEST_ASSERT_FALSE(BlobContainsText(blob, "AlphaIo"));
  TEST_ASSERT_FALSE(BlobContainsText(blob, "AuthFail"));
  TEST_ASSERT_FALSE(BlobContainsText(blob, "Info"));
  TEST_ASSERT_FALSE(BlobContainsText(blob, "test-space.cpp"));
  std::array<std::uint8_t, 4> checksum{};
  WriteU32LE(checksum.data(), space_a::Config::kSchemaChecksum);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(checksum.data(), blob.data() + 4, 4);
}

void test_RuntimeCountTypesAndDurationScope() {
  static_assert(std::is_same_v<space_a::Config::CountType, std::uint32_t>);
  static_assert(std::is_same_v<space_b::Config::CountType, std::uint64_t>);
  space_a::Clock::reset(0);
  space_a::Storage storage;
  space_a::Stream stream{storage, StreamMarker::kMain};
  {
    A_TELE_INFO(stream, space_a::kScope);
    space_a::Clock::advance(5000);
  }
  auto const* metric = storage.TryMetric(space_a::kScope.index);
  TEST_ASSERT_NOT_NULL(metric);
  if (metric == nullptr) {
    return;
  }
  TEST_ASSERT_EQUAL(1, metric->count);
  TEST_ASSERT_EQUAL_UINT64(5000, metric->sum);
  TEST_ASSERT_EQUAL_UINT64(5000, metric->min);
  TEST_ASSERT_EQUAL_UINT64(5000, metric->max);
}

void test_UserFreezeAndSwapAndSingleStorage() {
  space_a::Clock::reset(0);
  space_a::Storage single;
  space_a::Stream stream{single, StreamMarker::kMain};
  space_a::Clock::advance(1);
  EmitAPing(stream);
  auto const snapshot = SerializeBlob(single);
  single.Clear();
  TEST_ASSERT_EQUAL(0, single.log_bytes().size());
  TEST_ASSERT_FALSE(single.HasAnyMetric());
  auto decoded = DecodeBlob<space_a::Config>(snapshot);
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kOk),
                    static_cast<int>(decoded.status));

  space_a::Storage pair[2];
  space_a::Stream stream0{pair[0], StreamMarker::kMain};
  space_a::Clock::advance(3);
  EmitAPing(stream0);
  auto const frozen = SerializeBlob(pair[0]);
  pair[1].Clear();
  auto decoded_frozen = DecodeBlob<space_a::Config>(frozen);
  TEST_ASSERT_EQUAL(static_cast<int>(BlobStatus::kOk),
                    static_cast<int>(decoded_frozen.status));
  TEST_ASSERT_EQUAL(1, decoded_frozen.data->event_count);
}

void test_OldBlobRejectedByAlteredSchema() {
  constexpr auto altered_sum = [] {
    auto copy = space_a::Config::kTags;
    copy[0].index = 12345;
    return ComputeSchemaChecksum(
        kSpaceFormatVersion, kMetricsFormatVersion,
        space_a::Config::CountCodec::kSchemaSignature,
        space_a::Config::TimeCodec::kSchemaSignature,
        space_a::Config::DeltaCodec::kSchemaSignature,
        space_a::Config::IndexCodec::kSchemaSignature,
        space_a::Config::kTicksPerSecond, copy);
  }();
  TEST_ASSERT_NOT_EQUAL(space_a::Config::kSchemaChecksum, altered_sum);
}

}  // namespace ae::tele::test_space

namespace ae::tele::test_tele {
void test_SpaceChecksumIsUint32CompileTime() {
  test_space::test_SpaceChecksumIsUint32CompileTime();
}
void test_SpaceIndexChangeChangesChecksum() {
  test_space::test_SpaceIndexChangeChangesChecksum();
}
void test_TwoSpacesIndependentSameIndex() {
  test_space::test_TwoSpacesIndependentSameIndex();
}
void test_ExactDeltaAndCountRoundTrip() {
  test_space::test_ExactDeltaAndCountRoundTrip();
}
void test_BareUntimedRecordCanBeOneByte() {
  test_space::test_BareUntimedRecordCanBeOneByte();
}
void test_DisabledModuleCreatesNoData() {
  test_space::test_DisabledModuleCreatesNoData();
}
void test_BinaryOmitsPresentationStrings() {
  test_space::test_BinaryOmitsPresentationStrings();
}
void test_RuntimeCountTypesAndDurationScope() {
  test_space::test_RuntimeCountTypesAndDurationScope();
}
void test_UserFreezeAndSwapAndSingleStorage() {
  test_space::test_UserFreezeAndSwapAndSingleStorage();
}
void test_OldBlobRejectedByAlteredSchema() {
  test_space::test_OldBlobRejectedByAlteredSchema();
}
}  // namespace ae::tele::test_tele
