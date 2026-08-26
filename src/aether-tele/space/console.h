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

#ifndef AETHER_TELE_SPACE_CONSOLE_H_
#define AETHER_TELE_SPACE_CONSOLE_H_

// Host/debug path only. Do not include this header from embedded translation
// units: names, modules, and levels are stringified here.
#if !defined(AE_TELE_SPACE_HOST)
#error "space/console.h requires AE_TELE_SPACE_HOST"
#endif

#include <iostream>
#include <string_view>

#include "aether-tele/space/blob.h"
#include "aether-tele/space/schema.h"

namespace ae::tele::space {

inline void PrintDecodedEvent(DecodedEvent const& event,
                              std::string_view file = {},
                              int line = 0) {
  std::cout << '+' << event.rel_time << " ticks | stream="
            << MarkerName(event.stream) << " | idx=" << event.index;
  if (!event.name.empty()) {
    std::cout << " | " << event.name;
  }
  if (!event.module.empty()) {
    std::cout << " | module=" << event.module;
  }
  if (event.severity != 0) {
    std::cout << " | " << SeverityName(event.severity);
  }
  if (!file.empty()) {
    std::cout << " | " << file << ':' << line;
  }
  if (event.has_duration) {
    std::cout << " | dur=" << event.duration;
  }
  if (event.payload_fields > 0) {
    std::cout << " | p0=" << event.payload0;
  }
  std::cout << '\n';
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_CONSOLE_H_
