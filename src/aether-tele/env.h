#ifndef AETHER_TELE_ENV_H_
#define AETHER_TELE_ENV_H_

#include "aether-tele/config_consts.h"

#if defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN ||                 \
    defined(__BIG_ENDIAN__) || defined(__ARMEB__) || defined(__THUMBEB__) || \
    defined(__AARCH64EB__) || defined(_MIBSEB) || defined(__MIBSEB) ||       \
    defined(__MIBSEB__) ||                                                   \
    defined(Q_BYTE_ORDER) && Q_BYTE_ORDER == Q_BIG_ENDIAN
#  define AE_ENDIANNESS AE_BIG_ENDIAN
#elif defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN ||          \
    defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) ||                    \
    defined(__THUMBEL__) || defined(__AARCH64EL__) || defined(__i386__) || \
    defined(__amd64) || defined(__amd64__) || defined(_MIPSEL) ||          \
    defined(__MIPSEL) || defined(__MIPSEL__) || defined(ESP_PLATFORM) ||   \
    defined(Q_BYTE_ORDER) && Q_BYTE_ORDER == Q_LITTLE_ENDIAN
#  define AE_ENDIANNESS AE_LITTLE_ENDIAN
#else
#  define AE_ENDIANNESS AE_LITTLE_ENDIAN
#endif

#endif  // AETHER_TELE_ENV_H_
