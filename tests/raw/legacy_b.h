#ifndef TEST_RAW_LEGACY_B_H_
#define TEST_RAW_LEGACY_B_H_
#include "legacy_spaces.h"
inline void EmitB() { AE_TELE_INFO_TO(::routing::SinkB, ::routing::tag, "value {}", 7); }
#endif
