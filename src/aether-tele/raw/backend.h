/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_RAW_BACKEND_H_
#define AETHER_TELE_RAW_BACKEND_H_
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "aether-tele/itrap.h"
#include "aether-tele/raw/decode.h"

namespace ae::tele::raw {
namespace backend_internal {
inline thread_local unsigned callback_depth = 0;
}
// Shared across namespace identities: one backend may store several spaces.
// Nested telemetry must not enter another writer while a backend holds its own
// locks. Guarded operations reject such reentry before acquiring writer locks.
class BackendCallbackGuard {
 public:
  BackendCallbackGuard() noexcept { ++backend_internal::callback_depth; }
  ~BackendCallbackGuard() { --backend_internal::callback_depth; }
  BackendCallbackGuard(BackendCallbackGuard const&) = delete;
  BackendCallbackGuard& operator=(BackendCallbackGuard const&) = delete;
};
inline bool BackendCallbackActive() noexcept {
  return backend_internal::callback_depth != 0;
}

// Runtime storage owns accepted bytes. Append is all-or-retry; callbacks
// perform no object graph save/load. Restore runs before a new owner starts
// recording.
class IBackend {
 public:
  virtual ~IBackend() = default;
  virtual bool Append(std::string_view space,
                      std::span<std::uint8_t const> bytes) = 0;
  virtual bool RestoreMetrics(std::string_view space, std::uint32_t writer,
                              std::span<MetricTotal> output,
                              std::size_t& count) = 0;
  virtual void Console(std::string_view, std::span<std::uint8_t const>) {}
  virtual void WriteEnvData(std::string_view, ae::tele::EnvData const&) {}
  virtual std::vector<std::uint8_t> Snapshot(std::string_view) { return {}; }
};

// A bootstrap journal: preserves early accepted records before an application's
// persistent backend exists. It is intentionally separate from the raw core.
class MemoryBackend final : public IBackend {
 public:
  bool Append(std::string_view space,
              std::span<std::uint8_t const> bytes) override {
    auto decoded = Decode(bytes);
    if (decoded.status != DecodeStatus::kOk) return false;
    std::lock_guard lock(mutex_);
    auto& destination = spaces_[std::string{space}];
    destination.insert(destination.end(), bytes.begin(), bytes.end());
    return true;
  }
  bool RestoreMetrics(std::string_view space, std::uint32_t writer,
                      std::span<MetricTotal> output,
                      std::size_t& count) override {
    std::lock_guard lock(mutex_);
    count = 0;
    auto found = spaces_.find(std::string{space});
    if (found == spaces_.end()) return true;
    auto decoded = Decode(found->second);
    if (decoded.status != DecodeStatus::kOk) return false;
    std::map<std::uint32_t, Metric> totals;
    for (auto const& record : decoded.records)
      if (record.stream == writer && record.metric)
        AccumulateMetric(totals[record.index], *record.metric);
    if (totals.size() > output.size()) return false;
    for (auto const& [index, metric] : totals)
      output[count++] = {index, metric};
    return true;
  }
  std::vector<std::uint8_t> Snapshot(std::string_view space) override {
    std::lock_guard lock(mutex_);
    auto found = spaces_.find(std::string{space});
    return found == spaces_.end() ? std::vector<std::uint8_t>{} : found->second;
  }

 private:
  std::mutex mutex_;
  std::map<std::string, std::vector<std::uint8_t>> spaces_;
};
}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_BACKEND_H_
