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

#include <unity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ae-numeric/wire_io.h"

#include "aether-tele/packed_int.h"

namespace ae::tele::test_tele {

struct Golden {
  std::uint64_t value;
  std::uint8_t const* bytes;
  std::size_t size;
};

// Independent LE golden vectors for legacy TieredInt Limit=250 tiers.
static constexpr std::uint8_t k250[] = {0xfa};
static constexpr std::uint8_t k251[] = {0xfb, 0x00};
static constexpr std::uint8_t k1514[] = {0xff, 0xef};
static constexpr std::uint8_t k1515[] = {0xff, 0xf0, 0x00, 0x00};
static constexpr std::uint8_t k1049834[] = {0xff, 0xff, 0xff, 0xfe};
static constexpr std::uint8_t k1049835[] = {0xff, 0xff, 0x00, 0xff,
                                           0x00, 0x00, 0x00, 0x00};

static constexpr Golden kPackedU64Goldens[] = {
    {250, k250, sizeof(k250)},
    {251, k251, sizeof(k251)},
    {1514, k1514, sizeof(k1514)},
    {1515, k1515, sizeof(k1515)},
    {1049834, k1049834, sizeof(k1049834)},
    {1049835, k1049835, sizeof(k1049835)},
};

// PackedU32 is the three-tier legacy form (max wire 4 bytes).
static constexpr Golden kPackedU32Goldens[] = {
    {250, k250, sizeof(k250)},
    {251, k251, sizeof(k251)},
    {1514, k1514, sizeof(k1514)},
    {1515, k1515, sizeof(k1515)},
    {1049834, k1049834, sizeof(k1049834)},
};

template <typename T>
void AssertGoldenRoundTrip(Golden const& g) {
  std::array<std::uint8_t, MaxWireBytes<T>()> buf{};
  auto const n = ae::Serialize(T{g.value}, buf.data());
  TEST_ASSERT_EQUAL(g.size, n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(g.bytes, buf.data(), g.size);

  auto const decoded = ae::Deserialize<T>(buf.data(), n);
  TEST_ASSERT_EQUAL(g.size, decoded.bytes_read);
  TEST_ASSERT_EQUAL(static_cast<typename T::ValueType>(g.value),
                    static_cast<typename T::ValueType>(decoded.value));

  TEST_ASSERT_EQUAL(g.size, T::WireBytesNeeded(buf.data(), n));
  if (g.size > 1) {
    TEST_ASSERT_EQUAL(0, T::WireBytesNeeded(buf.data(), g.size - 1));
  }
}

void test_PackedU64WireGolden() {
  static_assert(PackedU64::kMaxWireBytes == 8);
  for (Golden const& g : kPackedU64Goldens) {
    AssertGoldenRoundTrip<PackedU64>(g);
  }
}

void test_PackedU32WireGolden() {
  static_assert(PackedU32::kMaxWireBytes == 4);
  for (Golden const& g : kPackedU32Goldens) {
    AssertGoldenRoundTrip<PackedU32>(g);
  }
}

}  // namespace ae::tele::test_tele
