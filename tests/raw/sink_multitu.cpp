/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include "sink_multitu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "aether-tele/raw/file.h"

namespace {
using namespace ae::tele::raw;
using sink_multitu::Sink;
void Check(bool value, char const* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
class Backend final : public IBackend {
 public:
  static void const* StaticTypeToken() noexcept {
    static constexpr char token{};
    return &token;
  }
  void const* TypeToken() const noexcept override { return StaticTypeToken(); }
  bool Append(std::string_view name,
              std::span<std::uint8_t const> bytes) override {
    std::lock_guard lock(attempts_mutex);
    ++attempts;
    attempted_bytes.emplace_back(bytes.begin(), bytes.end());
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    if (throw_append) throw 1;
#endif
    return accept && journal.Append(name, bytes);
  }
  bool RestoreMetrics(std::string_view name, std::uint32_t writer,
                      std::span<MetricTotal> output,
                      std::size_t& count) override {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    if (throw_restore) throw 2;
#endif
    return journal.RestoreMetrics(name, writer, output, count);
  }
  MemoryBackend journal;
  std::mutex attempts_mutex;
  std::vector<std::vector<std::uint8_t>> attempted_bytes;
  std::atomic<unsigned> attempts{};
  bool accept{true}, throw_append{}, throw_restore{};
};
// Exercises the templated file adapter under the same no-EH policy without
// native files or a separately compiled platform library.
struct MemoryFile {
  static constexpr bool kDirectorySyncSupported = false;
  bool open{}, fail_write{};
  std::vector<std::uint8_t> bytes;
  bool Open(std::filesystem::path const&, std::error_code&) {
    open = true;
    return true;
  }
  bool IsOpen() const { return open; }
  bool Close(std::error_code&) {
    open = false;
    return true;
  }
  bool Size(std::uint64_t& size, std::error_code&) {
    size = bytes.size();
    return true;
  }
  bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> out,
              std::size_t& read, std::error_code&) {
    if (offset > bytes.size()) return false;
    read =
        std::min(out.size(), bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(bytes.begin() + static_cast<std::size_t>(offset), read,
                out.begin());
    return true;
  }
  bool WriteAt(std::uint64_t offset, std::span<std::uint8_t const> data,
               std::size_t& written, std::error_code& error) {
    written = 0;
    if (fail_write) {
      error = std::make_error_code(std::errc::io_error);
      return false;
    }
    bytes.resize(static_cast<std::size_t>(offset) + data.size());
    std::copy(data.begin(), data.end(),
              bytes.begin() + static_cast<std::size_t>(offset));
    written = data.size();
    return true;
  }
  bool Truncate(std::uint64_t size, std::error_code&) {
    bytes.resize(static_cast<std::size_t>(size));
    return true;
  }
  bool Sync(std::error_code&) { return true; }
  bool SyncParent(std::error_code&) { return true; }
};
}  // namespace
int main() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink::Instance();
  Check(sink.SetBackend(backend), "attach backend");
  sink.SetRuntime({true, false, true, true, kTime | kDuration | kPayload});
  auto base = sink.GetBackend();
  Check(base->TypeToken() == Backend::StaticTypeToken(),
        "checked backend token");
  Check(std::static_pointer_cast<Backend>(base) == backend, "RTTI-free cast");
  Check(MemoryBackend{}.TypeToken() == nullptr, "default token unavailable");
  Check(sink_multitu::BindAndEmitA(11), "bind in TU A");
  Check(sink_multitu::EmitB() == 11, "TU B must reuse TU A's thread writer");
  backend->accept = false;
  Check(!sink.DrainAll(), "false callback retains pending data");
  auto const rejected = backend->attempted_bytes.back();
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
  backend->throw_append = true;
  Check(!sink.DrainAll(), "throwing callback retains pending data");
  Check(backend->attempted_bytes.back() == rejected,
        "exception retry preserves exact bytes");
#endif
  Check(backend->journal.Snapshot("multitu").empty(), "no partial acceptance");
  backend->accept = true;
  backend->throw_append = false;
  auto const retry_at = backend->attempted_bytes.size();
  Check(sink.DrainAll(), "retry accepted");
  Check(backend->attempted_bytes[retry_at] == rejected,
        "successful retry preserves exact bytes before metric drains");
  std::atomic<bool> correct{true};
  auto write = [&](std::uint32_t id) {
    if (!sink_multitu::BindAndEmitA(id) || sink_multitu::EmitB() != id)
      correct = false;
  };
  std::thread a(write, 21), b(write, 22);
  a.join();
  b.join();
  // A new owner of the same stable ID must restore its retired predecessor.
  std::thread reused(write, 21);
  reused.join();
  Check(correct, "per-thread cross-TU writer isolation");
  Check(sink.DrainAll(), "drain ended owners");
  auto bytes = backend->journal.Snapshot("multitu");
  auto decoded = Decode(bytes);
  Check(decoded.status == DecodeStatus::kOk, "decode all cross-TU events");
  std::size_t logs = 0;
  for (auto const& record : decoded.records)
    if (!record.metric) ++logs;
  Check(logs == 8, "all events retained exactly once");
  for (auto index : {1u, 3u}) {
    auto metric = RestoreMetric(decoded.records, index);
    Check(metric.count == 4 && metric.samples == 4, "exact cumulative metrics");
  }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
  backend->throw_restore = true;
  Check(!sink.BindWriter(31), "restore exception is contained");
  backend->throw_restore = false;
  Check(sink.BindWriter(31), "failed reservation released for retry");
  Check(!sink.SetBackend(backend, [](auto const&) -> bool { throw 3; }),
        "migration exception is contained");
  Check(sink.GetBackend() == backend, "failed migration preserves backend");
#endif
  MemoryFile io;
  BasicFileDrain file{io};
  Check(file.Open({}), "open file adapter");
  io.fail_write = true;
  Check(!file.Append(bytes) && io.bytes.empty(), "file write rollback");
  io.fail_write = false;
  Check(file.Append(bytes), "file retry");
  Check(file.Close() && file.Open({}), "reopen file adapter");
  Sink::SpaceType space;
  Stream<Sink::SpaceType, 4096> restored{space, 21};
  Check(file.RestoreMetrics(restored), "restore file statistics");
  Check(restored.MetricFor(1).count == 2 && restored.MetricFor(3).count == 2,
        "restored writer is independent and counted once");
  Check(file.Close(), "close file adapter");
  std::cout << "cross-TU writers, retry, restore and backend tokens passed\n";
}
