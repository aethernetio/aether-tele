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

#ifndef AETHER_TELE_SPACE_CLOCK_H_
#define AETHER_TELE_SPACE_CLOCK_H_

#include <atomic>
#include <cstdint>
#include <ctime>

namespace ae::tele::space {

// Deterministic tick clock. `Tag` keeps independent spaces from sharing now().
template <typename Time, typename Tag = void>
struct ManualClock {
  using time_type = Time;
  using TimeType = Time;

  static TimeType now() { return now_.load(std::memory_order_relaxed); }
  static void reset(TimeType value = TimeType{}) {
    now_.store(value, std::memory_order_relaxed);
  }
  static void advance(TimeType delta) {
    now_.fetch_add(delta, std::memory_order_relaxed);
  }

 private:
  static inline std::atomic<TimeType> now_{};
};

template <typename Tag = void>
struct ManualUnixClock {
  static std::uint32_t unix_seconds() {
    return unix_.load(std::memory_order_relaxed);
  }
  static void set(std::uint32_t value) {
    unix_.store(value, std::memory_order_relaxed);
  }
  static void advance(std::uint32_t delta) {
    unix_.fetch_add(delta, std::memory_order_relaxed);
  }

 private:
  static inline std::atomic<std::uint32_t> unix_{};
};

struct SystemUnixClock {
  static std::uint32_t unix_seconds() {
    auto const t = std::time(nullptr);
    if (t < 0) {
      return 0;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(t));
  }
};

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_CLOCK_H_
