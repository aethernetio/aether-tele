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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>

#include "demo/network_space.h"
#include "emit.h"

namespace {
std::atomic<std::size_t> g_allocs{0};
std::atomic<std::size_t> g_bytes{0};
std::atomic<std::size_t> g_live{0};
std::atomic<std::size_t> g_peak{0};
}  // namespace

void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(n, std::memory_order_relaxed);
  auto live = g_live.fetch_add(n, std::memory_order_relaxed) + n;
  std::size_t peak = g_peak.load(std::memory_order_relaxed);
  while (live > peak &&
         !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
  }
  void* p = std::malloc(n ? n : 1);
  if (p == nullptr) {
    throw std::bad_alloc{};
  }
  return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

int main() {
  using ae::tele::space::StreamMarker;
  auto const before_init = g_allocs.load();
  demo::network::Clock::reset(0);
  demo::network::Storage storage;
  auto const after_init = g_allocs.load();
  demo::network::Stream stream{storage, StreamMarker::kMain};

  auto const before = g_allocs.load();
  for (int i = 0; i < 1000; ++i) {
    demo::EmitPoll(stream);
    demo::network::Clock::advance(3);
  }
  auto const after_events = g_allocs.load();
  if (after_events != before) {
    std::cerr << "heap: hot path allocated " << (after_events - before)
              << " times\n";
    return 1;
  }

  auto const before_flush = g_allocs.load();
  auto blob = ae::tele::space::SerializeBlob(storage);
  auto const after_flush = g_allocs.load();
  std::cout << "heap_probe ok init_allocs=" << (after_init - before_init)
            << " event_allocs=0 flush_allocs="
            << (after_flush - before_flush) << " blob=" << blob.size()
            << " peak_live=" << g_peak.load() << " total_alloc_calls="
            << g_allocs.load() << " total_bytes=" << g_bytes.load() << '\n';
  (void)before_flush;
  return 0;
}
