#ifndef TEST_RAW_LEGACY_A_H_
#define TEST_RAW_LEGACY_A_H_
#include "legacy_spaces.h"
inline void EmitA() { AE_TELE_INFO_TO(::routing::SinkA, ::routing::tag, "value {}", 42); }
#endif
