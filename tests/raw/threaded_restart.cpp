/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/syscall.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#endif

#include "aether-tele/raw.h"
#include "aether-tele/raw/file.h"
#include "aether-tele/raw/metadata.h"

namespace {
using namespace ae::tele::raw;
// A reproducible workload, not a wall-clock performance benchmark. Each writer
// advances its timestamp by 1 ms/event and spends exactly 5 us in a scope.
struct Clock {
  static inline thread_local std::uint64_t timestamp{}, steady{};
  static std::uint64_t TimestampUs() { return timestamp; }
  static std::uint64_t SteadyUs() { return steady; }
};
struct Identity {};
using Telemetry = Space<Identity, Options{}, Clock>;
using Writer = Stream<Telemetry, 4096>;
AE_RAW_TAG(Telemetry, kWork, 7, 1, 2, "worker processes item");
constexpr Tag<Telemetry> tags[]{kWork};

void Require(bool condition, char const* message) {
  if (!condition) throw std::runtime_error(message);
}
unsigned Number(char const* text) {
  std::size_t end{};
  auto value = std::stoul(text, &end);
  Require(end == std::string{text}.size() && value <= 1'000'000,
          "invalid unsigned argument");
  return static_cast<unsigned>(value);
}
void CheckMetric(Writer const& stream, std::uint64_t count) {
  auto metric = stream.MetricFor(kWork.index);
  Require(metric.count == count && metric.samples == count &&
              metric.sum == count * 5 && metric.min == (count ? 5 : 0) &&
              metric.max == (count ? 5 : 0),
          "restored/live metric totals differ from expected history");
}
struct Result {
  std::string thread_id, error;
  std::uint64_t restored{}, native_thread_id{};
};
std::uint64_t NativeThreadId() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#elif defined(__linux__)
  return static_cast<std::uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
  std::uint64_t id{};
  Require(pthread_threadid_np(nullptr, &id) == 0,
          "native thread ID lookup failed");
  return id;
#else
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path capture, map;
    unsigned boot = 0, workers = 4, events = 1000, payload_size = 12;
    bool verify_only = false, abrupt_exit = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--verify-only")
        verify_only = true;
      else if (arg == "--abrupt-exit")
        abrupt_exit = true;
      else {
        Require(i + 1 < argc, "missing argument value");
        auto value = argv[++i];
        if (arg == "--capture")
          capture = value;
        else if (arg == "--map")
          map = value;
        else if (arg == "--boot")
          boot = Number(value);
        else if (arg == "--workers")
          workers = Number(value);
        else if (arg == "--events")
          events = Number(value);
        else if (arg == "--payload-bytes")
          payload_size = Number(value);
        else
          throw std::runtime_error("unknown argument: " + arg);
      }
    }
    Require(!capture.empty() && !map.empty(),
            "--capture and --map are required");
    Require(workers >= 1 && workers <= 32 && events >= 1 &&
                payload_size >= 12 && payload_size <= 2000,
            "workers must be 1..32, events >=1, payload-bytes 12..2000");
    FileDrain file;
    Require(file.Open(capture), "cannot open/validate capture");
    Telemetry space;
    space.SetRuntime({true, false, true, true, kTime | kDuration | kPayload});
    std::vector<std::unique_ptr<Writer>> streams;
    for (unsigned slot = 0; slot < workers; ++slot) {
      // Logical IDs belong to worker slots, never to OS thread IDs. Restore
      // every quiescent stream before allowing any writer to start.
      streams.push_back(std::make_unique<Writer>(space, slot + 1,
                                                 FileDrain::Callback, &file));
      Require(file.RestoreMetrics(*streams.back()), "metric restore failed");
      CheckMetric(*streams.back(), std::uint64_t{boot} * events);
    }
    if (verify_only) {
      auto size = file.size();
      for (auto const& stream : streams)
        Require(stream->DrainMetrics(), "empty restored drain failed");
      Require(file.size() == size, "restore re-emitted historical metrics");
      Require(file.Close(), "verify close failed");
      return 0;
    }
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<Result> results(workers);
    std::vector<std::thread> threads;
    // Change creation order across boots while retaining logical writer IDs.
    // Some runtimes use small per-process thread numbers instead of OS IDs.
    for (unsigned dummy = 0; dummy < boot % 3; ++dummy)
      std::thread([] {}).join();
    for (unsigned position = 0; position < workers; ++position) {
      unsigned const slot = (position + boot) % workers;
      threads.emplace_back([&, slot] {
        // Meet the barrier before fallible setup so a failed worker cannot
        // strand the others at startup.
        start.arrive_and_wait();
        try {
          auto& stream = *streams[slot];
          auto& result = results[slot];
          std::ostringstream id;
          id << std::this_thread::get_id();
          result.thread_id = id.str();
          result.native_thread_id = NativeThreadId();
          result.restored = stream.MetricFor(kWork.index).count;
          Clock::timestamp =
              1'700'000'000'000'000ULL + std::uint64_t{boot} * 2'000'000'000ULL;
          Clock::steady = 0;
          std::vector<std::uint8_t> payload(payload_size, 0xa5);
          Store32(payload.data(), boot);
          Store32(payload.data() + 4, slot + 1);
          for (unsigned item = 0; item < events; ++item) {
            Store32(payload.data() + 8, item);
            {
              // Payload schema: little-endian u32 boot, writer, item; optional
              // padding is 0xa5. All three fields are checked by the runner.
              Scope scope(stream, kWork, space.Runtime(), {payload});
              Require(RecordSite(kWork, __FILE__, __LINE__ - 1),
                      "callsite registration failed");
              Clock::steady += 5;
            }
            Clock::timestamp += 1000;
            if ((item + 1) % 127 == 0)
              Require(stream.DrainMetrics(), "owner checkpoint failed");
            if (item % 16 == 0) std::this_thread::yield();
          }
          CheckMetric(stream, std::uint64_t{boot + 1} * events);
          Require(stream.DrainMetrics(), "final owner checkpoint failed");
          Require(stream.dropped() == 0, "log records were dropped");
        } catch (std::exception const& error) {
          results[slot].error = error.what();
        }
      });
    }
    for (auto& thread : threads) thread.join();
    auto checkpoint_size = file.size();
    for (unsigned slot = 0; slot < workers; ++slot) {
      Require(results[slot].error.empty(), results[slot].error.c_str());
      Require(streams[slot]->DrainMetrics(), "repeated checkpoint failed");
    }
    Require(file.size() == checkpoint_size,
            "duplicate metric batch was written");
    {
      std::ofstream output(map);
      Require(
          ExportMap<Telemetry>(output, "threaded-restart", "fixture-v1", tags),
          "map export failed");
      output.close();
      Require(!output.fail(), "map close failed");
    }
    for (unsigned slot = 0; slot < workers; ++slot)
      std::cout << "{\"boot\":" << boot << ",\"writer\":" << slot + 1
                << ",\"thread_id\":\"" << results[slot].thread_id
                << "\",\"native_thread_id\":" << results[slot].native_thread_id
                << ",\"restored_count\":" << results[slot].restored
                << ",\"count\":" << streams[slot]->MetricFor(kWork.index).count
                << "}\n";
    std::cout.flush();
    // All batches are already synced. Skip destructors/Close on one boot to
    // prove that continued accumulation does not depend on shutdown hooks.
    if (abrupt_exit) std::_Exit(0);
    Require(file.Close(), "capture close failed");
    return 0;
  } catch (std::exception const& error) {
    std::cerr << "threaded restart fixture: " << error.what() << '\n';
    return 1;
  }
}
