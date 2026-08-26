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

#include <cstdint>
#include <iostream>

#include "demo/network_space.h"
#include "emit.h"

#if defined(_MSC_VER)
#pragma optimize("", off)
#endif

namespace {

void CaptureStackPtr(std::uintptr_t* out) {
  volatile char mark = 1;
  *out = reinterpret_cast<std::uintptr_t>(&mark);
  (void)mark;
}

}  // namespace

int main() {
  demo::network::Clock::reset(0);
  demo::network::Storage storage;
  demo::network::Stream stream{storage, ae::tele::space::StreamMarker::kMain};

  std::uintptr_t before_event = 0;
  CaptureStackPtr(&before_event);
  {
    demo::EmitPoll(stream);
    std::uintptr_t during_event = 0;
    CaptureStackPtr(&during_event);
    auto const event_span = before_event > during_event
                                ? before_event - during_event
                                : during_event - before_event;
    std::cout << "stack_probe event_span_bytes=" << event_span << '\n';
  }

  std::uintptr_t before_flush = 0;
  CaptureStackPtr(&before_flush);
  auto blob = ae::tele::space::SerializeBlob(storage);
  std::uintptr_t after_flush = 0;
  CaptureStackPtr(&after_flush);
  auto const flush_span = before_flush > after_flush
                              ? before_flush - after_flush
                              : after_flush - before_flush;
  std::cout << "stack_probe flush_span_bytes=" << flush_span
            << " blob=" << blob.size() << '\n';
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  std::cout << "stack_probe decode_status="
            << static_cast<int>(decoded.status) << '\n';
  return decoded.status == ae::tele::space::BlobStatus::kOk ? 0 : 1;
}
