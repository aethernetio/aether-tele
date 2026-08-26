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

#include <array>
#include "aether-tele/packed_int.h"
#include "aether-tele/space.h"

using namespace ae::tele::space;
using ae::tele::PackedU64;
using ae::tele::Level;

#define BAD_TAGS(X) \
  X(A, 4, Log, None, true, 1, Info) \
  X(B, 4, Log, None, true, 1, Info)

BAD_TAGS(AE_SPACE_POINT)

static constexpr auto kTags = std::array{BAD_TAGS(AE_SPACE_POINT_REF)};
static_assert(UniqueDirectIndices(kTags), "duplicate indices must fail");

int main() { return static_cast<int>(kA.index); }
