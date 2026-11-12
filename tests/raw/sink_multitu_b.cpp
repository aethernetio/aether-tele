/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include "sink_multitu.h"

AE_TELE_MODULE(module_b, 8, 3, 4);
AE_TAG_INDEXED(point_b, module_b, 0);
namespace sink_multitu {
std::uint32_t EmitB() {
  auto const id = Sink::Instance().CurrentWriterId();
  AE_TELE_INFO_TO(Sink, point_b, "from B {}", id);
  return id;
}
}  // namespace sink_multitu
