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

#ifndef AETHER_TELE_PACKED_INT_H_
#define AETHER_TELE_PACKED_INT_H_

#include <cstdint>

#include "ae-numeric/tiered_int.h"

namespace ae::tele {

// Legacy TieredInt<std::uint64_t, std::uint8_t, 250> wire encoding used for
// metric indices/counts. Byte-compatible with Aether PackedSize framing.
using PackedU64 = TieredInt<std::uint8_t, 250, 1514, 1049834>;

// Legacy TieredInt<std::uint32_t, std::uint8_t, 250> wire encoding used for
// compile-option indices (three tiers: 1/2/4 bytes).
using PackedU32 = TieredInt<std::uint8_t, 250, 1514>;

}  // namespace ae::tele

#endif  // AETHER_TELE_PACKED_INT_H_
