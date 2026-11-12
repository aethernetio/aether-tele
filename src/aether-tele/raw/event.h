/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_RAW_EVENT_H_
#define AETHER_TELE_RAW_EVENT_H_

#include <type_traits>
#include "aether-tele/raw/storage.h"

namespace ae::tele::raw {
template <typename StreamT>
void Log(StreamT& stream, Tag<typename StreamT::SpaceType> tag, Options options,
         Data data = {}) {
  using SpaceT = typename StreamT::SpaceType;
  options = Restrict(options, SpaceT::kCompiled);
  if (options.count) stream.Count(tag.index);
  if constexpr (SpaceT::kCompiled.logs || SpaceT::kCompiled.console) {
    if (options.logs || options.console) {
      std::uint64_t timestamp = 0;
      if constexpr ((SpaceT::kCompiled.fields & kTime) != 0) {
        if ((options.fields & kTime) != 0) {
          timestamp = SpaceT::ClockType::TimestampUs();
        }
      }
      // Preserve the configured block mask for instant events too: an untimed
      // event carries the unknown duration code instead of forcing a drain of
      // surrounding live scopes solely because their duration field is present.
      typename StreamT::DurationTicket unused;
      stream.Emit(tag, Kind::kLog, options, data, 0, timestamp, &unused);
    }
  }
}

// Flat inclusive duration. Reserve one byte at entry and patch it on exit only
// while the original block remains mutable. Drain permanently invalidates
// tickets.
template <typename StreamT>
class Scope {
 public:
  using SpaceT = typename StreamT::SpaceType;
  static constexpr bool kCanTime =
      SpaceT::kCompiled.timing ||
      ((SpaceT::kCompiled.logs || SpaceT::kCompiled.console) &&
       ((SpaceT::kCompiled.fields & kDuration) != 0));
  Scope(StreamT& stream, Tag<SpaceT> tag, Options options, Data data = {})
      : stream_(stream),
        tag_(tag),
        options_(Restrict(options, SpaceT::kCompiled)) {
    need_time_ = options_.timing || ((options_.logs || options_.console) &&
                                     ((options_.fields & kDuration) != 0));
    if constexpr (kCanTime) {
      if (need_time_) {
        stream_.BeginScope();
        start_ = SpaceT::ClockType::SteadyUs();
      }
    }
    if (options_.count) stream_.Count(tag_.index);
    if constexpr (SpaceT::kCompiled.logs || SpaceT::kCompiled.console) {
      if (options_.logs || options_.console) {
        std::uint64_t timestamp = 0;
        if constexpr ((SpaceT::kCompiled.fields & kTime) != 0) {
          if (options_.fields & kTime)
            timestamp = SpaceT::ClockType::TimestampUs();
        }
        stream_.Emit(tag_, Kind::kLog, options_, data, 0, timestamp, &ticket_);
      }
    }
  }
  Scope(Scope const&) = delete;
  Scope& operator=(Scope const&) = delete;
  Scope(Scope&&) = delete;
  ~Scope() noexcept {
    if constexpr (!kCanTime) {
      return;
    } else {
      if (!need_time_) return;
      auto const end = SpaceT::ClockType::SteadyUs();
      auto const duration = end >= start_ ? end - start_ : 0;
      if (options_.timing) stream_.AddDuration(tag_.index, duration);
      if ((options_.logs || options_.console) &&
          ((options_.fields & kDuration) != 0)) {
        stream_.FinishDuration(ticket_, duration);
      }
    }
  }

 private:
  StreamT& stream_;
  Tag<SpaceT> tag_;
  Options options_;
  typename StreamT::DurationTicket ticket_{};
  std::uint64_t start_{};
  bool need_time_{};
};
}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_EVENT_H_
