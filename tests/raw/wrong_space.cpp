/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
#include "aether-tele/raw.h"
struct A {}; struct B {};
using SpaceA = ae::tele::raw::Space<A>;
using SpaceB = ae::tele::raw::Space<B>;
AE_RAW_TAG(SpaceA, tag_a, 1, 1, 1, "same numeric index");
AE_RAW_TAG(SpaceB, tag_b, 1, 1, 1, "different space");
int main() {
  SpaceA a{};
  ae::tele::raw::Stream<SpaceA> stream(a, 0);
  AE_RAW_LOG(stream, tag_b); // Expected compile failure: foreign space tag.
}
