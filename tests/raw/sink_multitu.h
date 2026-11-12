/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_TESTS_RAW_SINK_MULTITU_H_
#define AETHER_TELE_TESTS_RAW_SINK_MULTITU_H_

#include <cstdint>
#include "aether-tele/configs/all_enabled.h"
#include "aether-tele/defines.h"
#include "aether-tele/raw/sink.h"

#if defined(AE_TEST_NO_EXCEPTIONS)
#  if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    error "The no-exceptions fixture must not inherit exception support"
#  endif
#  if defined(_CPPRTTI) || defined(__GXX_RTTI)
#    error "The no-RTTI fixture must not inherit RTTI support"
#  endif
#  if defined(_MSC_VER) && (!defined(_HAS_EXCEPTIONS) || _HAS_EXCEPTIONS != 0)
#    error "The no-exceptions fixture requires the MSVC no-EH STL policy"
#  endif
#endif

namespace sink_multitu {
struct Identity {
  static constexpr std::string_view kName = "multitu";
  static std::uint32_t ResolveIndex(std::string_view, std::uint32_t,
                                    ae::tele::Tag const& tag) {
    return tag.index();
  }
};
using Sink = ae::tele::RawTeleSink<Identity, ae::tele::AllEnabledConfig>;
bool BindAndEmitA(std::uint32_t id);
std::uint32_t EmitB();
}  // namespace sink_multitu
#endif  // AETHER_TELE_TESTS_RAW_SINK_MULTITU_H_
