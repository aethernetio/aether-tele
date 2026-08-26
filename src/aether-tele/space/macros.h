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

#ifndef AETHER_TELE_SPACE_MACROS_H_
#define AETHER_TELE_SPACE_MACROS_H_

#include <type_traits>

#include "aether-tele/levels.h"
#include "aether-tele/space/event.h"

#define AE_SPACE_CAT_(A, B) A##B
#define AE_SPACE_CAT(A, B) AE_SPACE_CAT_(A, B)

#define AE_SPACE_POINT(NAME, INDEX, TYPE, PAYLOAD, TIMED, MODULE_ID, SEVERITY) \
  inline constexpr ::ae::tele::space::Tag k##NAME{                             \
      static_cast<std::uint32_t>(INDEX),                                       \
      ::ae::tele::space::RecordType::k##TYPE,                                  \
      ::ae::tele::space::PayloadKind::k##PAYLOAD,                              \
      ::ae::tele::space::FlagsFor(::ae::tele::space::RecordType::k##TYPE,      \
                                  static_cast<bool>(TIMED)),                   \
      ::ae::tele::Level::k##SEVERITY,                                          \
      static_cast<std::uint32_t>(MODULE_ID)};

#define AE_SPACE_POINT_REF(NAME, INDEX, TYPE, PAYLOAD, TIMED, MODULE_ID, \
                           SEVERITY)                                     \
  k##NAME,

#if defined(AE_TELE_SPACE_HOST)
#define AE_SPACE_HOST_META(NAME, INDEX, TYPE, PAYLOAD, TIMED, MODULE_ID, \
                           MODULE_NAME, SEVERITY)                        \
  ::ae::tele::space::HostTag{static_cast<std::uint32_t>(INDEX), #NAME,   \
                             MODULE_NAME, ::ae::tele::Level::k##SEVERITY},
#endif

#ifdef AE_TELE_SPACE_COMPILE_OUT
#define AE_SPACE_TELE_DEBUG(STREAM, TAG, ...) ((void)sizeof(STREAM))
#define AE_SPACE_TELE_INFO(STREAM, TAG, ...) ((void)sizeof(STREAM))
#define AE_SPACE_TELE_WARNING(STREAM, TAG, ...) ((void)sizeof(STREAM))
#define AE_SPACE_TELE_ERROR(STREAM, TAG, ...) ((void)sizeof(STREAM))
#else

#define AE_SPACE_TELE_IMPL(STREAM, TAG, LEVELV, ...)                          \
  [[maybe_unused]] std::conditional_t<                                        \
      ::ae::tele::space::StreamEnabled<                                       \
          std::remove_cvref_t<decltype(STREAM)>, (TAG), LEVELV>(),            \
      ::ae::tele::space::Event<std::remove_cvref_t<decltype(STREAM)>, (TAG),  \
                               LEVELV>,                                       \
      ::ae::tele::space::DisabledEvent>                                       \
      AE_SPACE_CAT(_ae_space_evt_, __LINE__) {                                \
    (STREAM) __VA_OPT__(, ) __VA_ARGS__                                       \
  }

#define AE_SPACE_TELE_DEBUG(STREAM, TAG, ...)                      \
  static_assert((TAG).severity == ::ae::tele::Level::kDebug,       \
                #TAG " is not declared as Debug");                 \
  AE_SPACE_TELE_IMPL(STREAM, TAG, ::ae::tele::Level::kDebug, __VA_ARGS__)

#define AE_SPACE_TELE_INFO(STREAM, TAG, ...)                     \
  static_assert((TAG).severity == ::ae::tele::Level::kInfo,      \
                #TAG " is not declared as Info");                \
  AE_SPACE_TELE_IMPL(STREAM, TAG, ::ae::tele::Level::kInfo, __VA_ARGS__)

#define AE_SPACE_TELE_WARNING(STREAM, TAG, ...)                       \
  static_assert((TAG).severity == ::ae::tele::Level::kWarning,        \
                #TAG " is not declared as Warning");                  \
  AE_SPACE_TELE_IMPL(STREAM, TAG, ::ae::tele::Level::kWarning, __VA_ARGS__)

#define AE_SPACE_TELE_ERROR(STREAM, TAG, ...)                      \
  static_assert((TAG).severity == ::ae::tele::Level::kError,       \
                #TAG " is not declared as Error");                 \
  AE_SPACE_TELE_IMPL(STREAM, TAG, ::ae::tele::Level::kError, __VA_ARGS__)

#endif

#endif  // AETHER_TELE_SPACE_MACROS_H_
