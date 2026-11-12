/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_RAW_MACROS_H_
#define AETHER_TELE_RAW_MACROS_H_

#include "aether-tele/raw/event.h"
#if defined(AE_TELE_RAW_DEBUG) && AE_TELE_RAW_DEBUG
#  include "aether-tele/raw/metadata.h"
#endif

namespace ae::tele::raw {
// Factories are created at the caller, so internal variable names cannot shadow
// user expressions. Payload owners live through the complete encode operation.
template <typename StreamT, typename TagT, typename PayloadFactory,
          typename Register>
void LazyLog(StreamT& stream, TagT tag, PayloadFactory&& payload,
             Register&& register_site) {
  using SpaceT = typename StreamT::SpaceType;
  static_assert(std::is_same_v<typename TagT::SpaceType, SpaceT>,
                "telemetry tag belongs to another space");
  register_site(tag);
  auto const options = stream.space().Runtime();
  Data data{};
  auto commit = [&](auto&& bytes) {
    data.payload = bytes;
    Log(stream, tag, options, data);
  };
  if constexpr ((SpaceT::kCompiled.fields & kPayload) != 0 &&
                (SpaceT::kCompiled.logs || SpaceT::kCompiled.console)) {
    if ((options.logs || options.console) &&
        ((options.fields & kPayload) != 0)) {
      commit(payload());
    } else {
      commit(std::span<std::uint8_t const>{});
    }
  } else {
    commit(std::span<std::uint8_t const>{});
  }
}

template <typename StreamT, typename TagT, typename Register>
auto MakeScope(StreamT& stream, TagT tag, Register&& register_site) {
  static_assert(
      std::is_same_v<typename TagT::SpaceType, typename StreamT::SpaceType>,
      "telemetry tag belongs to another space");
  register_site(tag);
  return Scope(stream, tag, stream.space().Runtime());
}
}  // namespace ae::tele::raw

#define AE_RAW_CAT_(A, B) A##B
#define AE_RAW_CAT(A, B) AE_RAW_CAT_(A, B)
#define AE_RAW_TAG(SPACE, NAME, INDEX, MODULE, LEVEL, TEXT)  \
  static_assert((INDEX) >= 0 && (INDEX) <= 65535,            \
                "telemetry tag index exceeds 16-bit range"); \
  inline constexpr ::ae::tele::raw::Tag<SPACE> NAME {        \
    INDEX, MODULE, LEVEL, #NAME, TEXT, __FILE__, __LINE__    \
  }

#if defined(AE_TELE_RAW_DEBUG) && AE_TELE_RAW_DEBUG
#  define AE_RAW_REGISTER_SITE                                    \
    [](auto const& point) {                                       \
      [[maybe_unused]] static bool const registered =             \
          ::ae::tele::raw::RecordSite(point, __FILE__, __LINE__); \
    }
#else
#  define AE_RAW_REGISTER_SITE [](auto const&) {}
#endif

#define AE_RAW_LOG_IMPL(STREAM, TAG, PAYLOAD)                             \
  ::ae::tele::raw::LazyLog((STREAM), (TAG),                               \
                           [&]() -> decltype(auto) { return (PAYLOAD); }, \
                           AE_RAW_REGISTER_SITE)
#define AE_RAW_LOG(STREAM, TAG) \
  AE_RAW_LOG_IMPL(STREAM, TAG, (std::span<std::uint8_t const>{}))
#define AE_RAW_PAYLOAD(STREAM, TAG, PAYLOAD) \
  AE_RAW_LOG_IMPL(STREAM, TAG, PAYLOAD)
#define AE_RAW_SCOPE(STREAM, TAG)                                \
  [[maybe_unused]] auto AE_RAW_CAT(ae_raw_scope_, __COUNTER__) = \
      ::ae::tele::raw::MakeScope((STREAM), (TAG), AE_RAW_REGISTER_SITE)

#endif  // AETHER_TELE_RAW_MACROS_H_
