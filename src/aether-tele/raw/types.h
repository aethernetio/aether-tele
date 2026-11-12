/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_RAW_TYPES_H_
#define AETHER_TELE_RAW_TYPES_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace ae::tele::raw {

enum Field : std::uint32_t {
  kTime = 1u << 0,
  kDuration = 1u << 1,
  kPayload = 1u << 3,
};
// Removed bits are invalid; never reinterpret an old capture's fields.
inline constexpr std::uint32_t kAllFields = kTime | kDuration | kPayload;
// Smallest block header: size:1 byte, control:u8, stream:1 byte, sequence:2
// bytes.
inline constexpr std::size_t kHeaderSize = 5;
// Every block has at least one row, whose smallest representation is one tag.
inline constexpr std::size_t kMinBlockSize = kHeaderSize + 1;
enum class Kind : std::uint32_t { kLog, kMetric };

enum class DurationState { kBelowMin, kValue, kAboveMax, kUnknown };
struct DurationValue {
  DurationState state{DurationState::kUnknown};
  std::uint64_t microseconds{};
  bool known() const noexcept { return state == DurationState::kValue; }
};

struct Options {
  bool logs = true;
  bool console = true;
  bool count = true;
  bool timing = true;
  std::uint32_t fields = kAllFields;
  constexpr bool operator==(Options const&) const = default;
};
constexpr Options Restrict(Options value, Options ceiling) {
  return {value.logs && ceiling.logs, value.console && ceiling.console,
          value.count && ceiling.count, value.timing && ceiling.timing,
          value.fields & ceiling.fields & kAllFields};
}
constexpr std::uint32_t PackOptions(Options o) {
  return (o.fields & kAllFields) | (std::uint32_t{o.logs} << 8) |
         (std::uint32_t{o.console} << 9) | (std::uint32_t{o.count} << 10) |
         (std::uint32_t{o.timing} << 11);
}
constexpr Options UnpackOptions(std::uint32_t o) {
  return {bool(o & (1u << 8)), bool(o & (1u << 9)), bool(o & (1u << 10)),
          bool(o & (1u << 11)), o & kAllFields};
}
struct SystemClock {
  static std::uint64_t TimestampUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
  }
  static std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
};
template <typename Identity, Options Compiled = Options{},
          typename Clock = SystemClock>
class Space {
 public:
  using IdentityType = Identity;
  using ClockType = Clock;
  static constexpr Options kCompiled = Compiled;
  Space() = default;
  Space(Space const&) = delete;
  Space& operator=(Space const&) = delete;
  void SetRuntime(Options options) {
    runtime_.store(PackOptions(Restrict(options, Compiled)),
                   std::memory_order_release);
  }
  Options Runtime() const {
    return UnpackOptions(runtime_.load(std::memory_order_acquire));
  }

 private:
  std::atomic<std::uint32_t> runtime_{
      PackOptions(Restrict(Compiled, Compiled))};
};
template <typename SpaceT>
struct Tag {
  using SpaceType = SpaceT;
  std::uint32_t index;
  std::uint32_t module;
  std::uint32_t level;
  std::string_view name;
  std::string_view text;
  std::string_view file;
  std::uint32_t line;
};
template <typename TagT, std::size_t N>
constexpr bool UniqueIndices(TagT const (&tags)[N]) {
  for (std::size_t i = 0; i < N; ++i)
    for (std::size_t j = i + 1; j < N; ++j)
      if (tags[i].index == tags[j].index) return false;
  return true;
}
struct Data {
  std::span<std::uint8_t const> payload{};
};
struct Metric {
  std::uint64_t count{};
  std::uint64_t samples{};
  std::uint64_t sum{};
  std::uint64_t min{};
  std::uint64_t max{};
};
// RAM restoration input. These totals are already persisted, not new deltas.
struct MetricTotal {
  std::uint32_t index{};
  Metric metric{};
};
inline void AccumulateMetric(Metric& target, Metric const& source) noexcept {
  auto add = [](std::uint64_t a, std::uint64_t b) {
    auto const limit = std::numeric_limits<std::uint64_t>::max();
    return b > limit - a ? limit : a + b;
  };
  target.count = add(target.count, source.count);
  target.sum = add(target.sum, source.sum);
  if (source.samples != 0) {
    if (target.samples == 0) {
      target.min = source.min;
      target.max = source.max;
    } else {
      target.min = std::min(target.min, source.min);
      target.max = std::max(target.max, source.max);
    }
  }
  target.samples = add(target.samples, source.samples);
}
// Callbacks run synchronously on the writer thread, outside any library lock.
// Drain success acknowledges the entire span. Failure retains its exact bytes.
using DrainCallback = bool (*)(void*, std::span<std::uint8_t const>);
using ConsoleCallback = void (*)(void*, std::span<std::uint8_t const>);

// Debug-map strings use UTF-8. Event payload bytes are opaque.
inline bool ValidUtf8(std::span<std::uint8_t const> bytes) {
  for (std::size_t i = 0; i < bytes.size();) {
    auto const lead = bytes[i++];
    if (lead <= 0x7f) continue;
    unsigned following{};
    std::uint8_t lower = 0x80, upper = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf)
      following = 1;
    else if (lead >= 0xe0 && lead <= 0xef) {
      following = 2;
      if (lead == 0xe0) lower = 0xa0;
      if (lead == 0xed) upper = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      following = 3;
      if (lead == 0xf0) lower = 0x90;
      if (lead == 0xf4) upper = 0x8f;
    } else
      return false;
    auto const available = bytes.size() - i;
    if (following > available) return false;
    if (available != 0 && (bytes[i] < lower || bytes[i] > upper)) return false;
    for (unsigned j = 1; j < following && j < available; ++j)
      if (bytes[i + j] < 0x80 || bytes[i + j] > 0xbf) return false;
    i += following;
  }
  return true;
}
inline bool ValidUtf8(std::string_view text) {
  return ValidUtf8(std::span<std::uint8_t const>{
      reinterpret_cast<std::uint8_t const*>(text.data()), text.size()});
}

inline void Store32(std::uint8_t* out, std::uint32_t v) {
  for (unsigned i = 0; i != 4; ++i)
    out[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void Store64(std::uint8_t* out, std::uint64_t v) {
  for (unsigned i = 0; i != 8; ++i)
    out[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint32_t Load32(std::uint8_t const* in) {
  std::uint32_t v = 0;
  for (unsigned i = 0; i != 4; ++i) v |= std::uint32_t{in[i]} << (8 * i);
  return v;
}
inline std::uint64_t Load64(std::uint8_t const* in) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i != 8; ++i) v |= std::uint64_t{in[i]} << (8 * i);
  return v;
}
}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_TYPES_H_
