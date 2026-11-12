/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include "sink_multitu.h"

AE_TELE_MODULE(module_a, 7, 1, 2);
AE_TAG_INDEXED(point_a, module_a, 0);
namespace sink_multitu {
bool BindAndEmitA(std::uint32_t id) {
  if (!Sink::Instance().BindWriter(id)) return false;
  AE_TELE_INFO_TO(Sink, point_a, "from A {}", id);
  return true;
}
}  // namespace sink_multitu
