/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#ifndef AETHER_TELE_RAW_SINK_H_
#define AETHER_TELE_RAW_SINK_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "aether-tele/collectors.h"
#include "aether-tele/raw/backend.h"
#include "aether-tele/raw/event.h"
#include "aether-tele/raw/metadata.h"

namespace ae::tele {
namespace raw_sink_internal {
constexpr raw::Options OptionsFor(TeleConfig config) {
  return {config.logs_enabled, config.logs_enabled, config.count_metrics,
          config.time_metrics,
          (config.start_time_logs ? raw::kTime : 0u) |
              (config.time_metrics ? raw::kDuration : 0u) |
              (config.blob_logs ? raw::kPayload : 0u)};
}
template <typename Sink, TeleConfig Config>
class Event;
}  // namespace raw_sink_internal

// Each identity is an independent telemetry namespace. Configured streams have
// one owner thread. Checkpoints serialize with owners, and the binding gate
// makes backend replacement atomic with event entry and completion. Explicit
// IDs are recommended for stable per-writer statistics across restarts.
template <typename Identity, ConfigProvider CP, std::size_t Capacity = 4096,
          std::size_t MetricCapacity = 128>
class RawTeleSink {
 public:
  using ConfigProviderType = CP;
  using SpaceType = raw::Space<Identity>;
  using StreamType = raw::Stream<SpaceType, Capacity, MetricCapacity>;
  static constexpr std::string_view kName = Identity::kName;
  template <Level::underlined_t L, std::uint32_t M>
  static consteval auto GetTeleConfig() {
    return CP::template GetTeleConfig<L, M>();
  }
  static consteval auto GetEnvConfig() { return CP::GetEnvConfig(); }
  static RawTeleSink& Instance() {
    static RawTeleSink sink;
    return sink;
  }

  std::shared_ptr<raw::IBackend> GetBackend() const {
    return sink_internal::LoadShared(backend_);
  }
  // Migration runs under the exclusive binding gate after old writers drain.
  // Failure preserves the binding and all unacknowledged pending bytes. Open
  // scopes retain their streams; their eventual duration deltas reach the new
  // backend through the redirected callback. Joining owners and committing the
  // application archive are still required for a durable shutdown.
  bool SetBackend(std::shared_ptr<raw::IBackend> backend,
                  std::function<bool(std::shared_ptr<raw::IBackend> const&)>
                      migrate = {}) noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return false;
    }
    std::unique_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      auto writers = Writers();
      for (auto const& writer : writers) {
        std::lock_guard lock(writer->mutex);
        if (writer->ready && !writer->stream.DrainMetrics()) return false;
        writer->clean = true;
      }
      if (migrate && !migrate(GetBackend())) return false;
      for (auto const& writer : writers) {
        std::lock_guard lock(writer->mutex);
        sink_internal::StoreShared(writer->backend, backend);
        writer->retired.store(true, std::memory_order_release);
      }
      sink_internal::StoreShared(backend_, std::move(backend));
      writers.clear();
      CollectRetired();
      return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++failures_;
      return false;
    }
#endif
  }
  void SetRuntime(raw::Options options) { space_.SetRuntime(options); }
  raw::Options Runtime() const { return space_.Runtime(); }
  std::uint64_t failures() const { return failures_.load(); }
  bool BindWriter(std::uint32_t id) noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return false;
    }
    std::shared_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
    if (id > raw::wire::kMaxStream) return false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      auto previous = Current().writer.lock();
      if (previous && !previous->retired.load() && previous->stream.id() == id)
        return true;
      auto next = CreateWriter(id);
      if (!next) return false;
      if (previous) {
        std::lock_guard lock(previous->mutex);
        if (!previous->stream.DrainMetrics()) {
          RemoveWriter(next);
          return false;
        }
        previous->clean = true;
        previous->retired.store(true);
      }
      Current().writer = next;
      Current().id = id;
      Current().assigned = true;
      return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++failures_;
      return false;
    }
#endif
  }
  bool DrainCurrent() noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return false;
    }
    std::shared_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
    auto writer = Current().writer.lock();
    if (!writer) return true;
    std::lock_guard lock(writer->mutex);
    auto const saved = writer->stream.DrainMetrics();
    if (saved) writer->clean = true;
    return saved;
  }
  bool DrainAll() noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return false;
    }
    std::shared_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      auto writers = Writers();
      for (auto const& writer : writers) {
        std::lock_guard lock(writer->mutex);
        if (writer->ready && !writer->stream.DrainMetrics()) return false;
        writer->clean = true;
      }
      writers.clear();
      CollectRetired();
      return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++failures_;
      return false;
    }
#endif
  }
  std::uint32_t CurrentWriterId() noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return raw::wire::kMaxStream + 1;
    }
    std::shared_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
    auto writer = AcquireWriter();
    return writer ? writer->stream.id() : raw::wire::kMaxStream + 1;
  }
  raw::Metric MetricFor(std::uint32_t index) noexcept {
    if (raw::BackendCallbackActive()) {
      ++failures_;
      return {};
    }
    // Freeze registration as well as entry/completion: otherwise a newly
    // registered writer could acknowledge a delta between snapshot and query.
    std::unique_lock binding(binding_gate_);
    raw::BackendCallbackGuard operation;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!Current().assigned && !AcquireWriter()) return {};
      auto writers = Writers();
      writers.erase(std::remove_if(writers.begin(), writers.end(),
                                   [&](auto const& writer) {
                                     return writer->stream.id() != Current().id;
                                   }),
                    writers.end());
      std::vector<std::unique_lock<std::recursive_mutex>> locks;
      for (auto const& writer : writers) locks.emplace_back(writer->mutex);
      raw::Metric result;
      auto backend = GetBackend();
      if (backend) {
        std::array<raw::MetricTotal, MetricCapacity> totals{};
        std::size_t count = 0;
        if (!backend->RestoreMetrics(kName, Current().id, totals, count) ||
            count > totals.size()) {
          ++failures_;
          return {};
        }
        bool found = false;
        for (auto const& total : std::span{totals}.first(count)) {
          if (total.index > raw::wire::kMaxTag ||
              (total.metric.samples && total.metric.min > total.metric.max)) {
            ++failures_;
            return {};
          }
          if (total.index != index) continue;
          if (found) {
            ++failures_;
            return {};
          }
          found = true;
          result = total.metric;
        }
      }
      for (auto const& writer : writers)
        raw::AccumulateMetric(result, writer->stream.PendingMetricFor(index));
      return result;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++failures_;
      return {};
    }
#endif
  }
  std::shared_ptr<ITrap> GetTrap() const {
    auto legacy = sink_internal::LoadShared(legacy_trap_);
    return legacy ? legacy : env_bridge_;
  }
  void SetTrap(std::shared_ptr<ITrap> trap) {
    sink_internal::StoreShared(legacy_trap_, std::move(trap));
  }
  // Retained for source-level environment integration; owning GetTrap() is the
  // synchronized API. Raw event recording always uses GetBackend().
  std::shared_ptr<ITrap> trap() const { return GetTrap(); }

  bool ExportMap(std::ostream& output, std::string_view build) const {
    std::map<std::uint32_t, Site> sites;
    {
      std::lock_guard lock(sites_mutex_);
      if (!sites_valid_) return false;
      sites = sites_;
    }
    std::vector<raw::Tag<SpaceType>> tags;
    for (auto const& [index, site] : sites) tags.push_back(site.Tag(index));
    std::ostringstream serialized;
    if (!raw::ExportMap(serialized, kName, build,
                        std::span<raw::Tag<SpaceType> const>{tags}))
      return false;
    auto text = serialized.str();
#if defined(AE_TELE_RAW_DEBUG) && AE_TELE_RAW_DEBUG
    // This front-end owns a dynamic registry of every observed callsite. Avoid
    // the fixed embedded raw-macro registry's 1024-site limit for host users.
    auto const key = std::string_view{"\"observed_callsites\":[]"};
    auto const position = text.find(key);
    if (position == std::string::npos) return false;
    std::ostringstream observed;
    observed << "\"observed_callsites\":[";
    bool separator = false;
    for (auto const& tag : tags) {
      if (separator) observed << ',';
      separator = true;
      raw::metadata_internal::WriteTag(observed, tag);
    }
    observed << ']';
    text.replace(position, key.size(), observed.str());
#endif
    text.insert(text.rfind('}'), ",\n\"payload_encoding\":\"utf8\"\n");
    output << text;
    return static_cast<bool>(output);
  }

 private:
  template <typename, TeleConfig>
  friend class raw_sink_internal::Event;
  struct Writer {
    Writer(SpaceType& space, std::uint32_t id,
           std::shared_ptr<raw::IBackend> target)
        : backend(std::move(target)),
          stream(space, id, Append, this, Console, this),
          owner(std::this_thread::get_id()) {}
    static bool Append(void* context, std::span<std::uint8_t const> bytes) {
      auto& self = *static_cast<Writer*>(context);
      auto target = sink_internal::LoadShared(self.backend);
      raw::BackendCallbackGuard guard;
      return target && target->Append(kName, bytes);
    }
    static void Console(void* context, std::span<std::uint8_t const> bytes) {
      auto& self = *static_cast<Writer*>(context);
      auto target = sink_internal::LoadShared(self.backend);
      raw::BackendCallbackGuard guard;
      if (target) target->Console(kName, bytes);
    }
    std::shared_ptr<raw::IBackend> backend;
    StreamType stream;
    std::thread::id owner;
    std::recursive_mutex mutex;
    std::atomic<bool> retired{false}, owner_exited{false}, ready{false},
        clean{true};
  };
  struct Cache {
    std::weak_ptr<Writer> writer;
    std::uint32_t id{};
    bool assigned{};
    ~Cache() {
      if (auto current = writer.lock()) {
        current->owner_exited.store(true, std::memory_order_release);
        current->retired.store(true, std::memory_order_release);
      }
    }
  };
  struct Site {
    std::uint32_t module{}, level{}, line{};
    std::string name, text, file;
    raw::Tag<SpaceType> Tag(std::uint32_t index) const {
      return {index, module, level, name, text, file, line};
    }
  };
  class EnvBridge final : public ITrap {
   public:
    explicit EnvBridge(RawTeleSink& sink) : sink_(sink) {}
    void AddInvoke(Tag const&, std::uint32_t) override {}
    void AddInvokeDuration(Tag const&, Duration) override {}
    void LogLine(Tag const&, ILogCollector&) override {}
    void WriteEnvData(EnvData const& data) override {
      if (raw::BackendCallbackActive()) {
        ++sink_.failures_;
        return;
      }
      std::shared_lock binding(sink_.binding_gate_);
      raw::BackendCallbackGuard guard;
      auto backend = sink_.GetBackend();
      if (backend) backend->WriteEnvData(kName, data);
    }

   private:
    RawTeleSink& sink_;
  };
  static std::shared_ptr<raw::IBackend> MakeBackend() {
    if constexpr (requires { CP::MakeBackend(); })
      return CP::MakeBackend();
    else
      return std::make_shared<raw::MemoryBackend>();
  }
  RawTeleSink()
      : backend_(MakeBackend()),
        env_bridge_(std::make_shared<EnvBridge>(*this)) {}
  std::vector<std::shared_ptr<Writer>> Writers() const {
    std::lock_guard lock(writers_mutex_);
    return writers_;
  }
  void RemoveWriter(std::shared_ptr<Writer> const& writer) {
    std::lock_guard lock(writers_mutex_);
    writers_.erase(std::remove(writers_.begin(), writers_.end(), writer),
                   writers_.end());
  }
  void CollectRetired() {
    std::lock_guard lock(writers_mutex_);
    writers_.erase(std::remove_if(writers_.begin(), writers_.end(),
                                  [](auto const& writer) {
                                    return writer.use_count() == 1 &&
                                           writer->retired.load() &&
                                           writer->clean.load();
                                  }),
                   writers_.end());
  }
  std::shared_ptr<Writer> CreateWriter(std::uint32_t id) {
    auto backend = GetBackend();
    if (!backend) return {};
    auto writer = std::make_shared<Writer>(space_, id, backend);
    std::vector<std::shared_ptr<Writer>> predecessors;
    {
      std::lock_guard lock(writers_mutex_);
      for (auto const& existing : writers_) {
        if (existing->stream.id() != id) continue;
        if (existing->retired.load() &&
            (existing->owner == std::this_thread::get_id() ||
             existing->owner_exited.load())) {
          predecessors.push_back(existing);
          continue;
        }
        if (existing->owner == std::this_thread::get_id() && existing->ready)
          return existing;
        ++failures_;
        return {};
      }
      writers_.push_back(writer);  // reserve this live ID before user code
    }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      for (auto const& previous : predecessors) {
        std::lock_guard lock(previous->mutex);
        if (!previous->stream.DrainMetrics()) {
          RemoveWriter(writer);
          ++failures_;
          return {};
        }
        previous->clean = true;
      }
      std::lock_guard stream_lock(writer->mutex);
      std::array<raw::MetricTotal, MetricCapacity> totals{};
      std::size_t count = 0;
      raw::BackendCallbackGuard guard;
      if (!backend->RestoreMetrics(kName, id, totals, count) ||
          count > totals.size() ||
          !writer->stream.RestoreMetrics(std::span{totals}.first(count))) {
        RemoveWriter(writer);
        ++failures_;
        return {};
      }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      RemoveWriter(writer);
      ++failures_;
      return {};
    }
#endif
    {
      std::lock_guard lock(writers_mutex_);
      writer->ready = true;
    }
    return writer;
  }
  std::shared_ptr<Writer> AcquireWriter() noexcept {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (auto writer = Current().writer.lock();
          writer && !writer->retired.load())
        return writer;
      if (!Current().assigned) {
        std::lock_guard lock(writers_mutex_);
        while (next_writer_ <= raw::wire::kMaxStream &&
               std::any_of(writers_.begin(), writers_.end(),
                           [&](auto const& writer) {
                             return !writer->retired.load() &&
                                    writer->stream.id() == next_writer_;
                           }))
          ++next_writer_;
        if (next_writer_ > raw::wire::kMaxStream) {
          ++failures_;
          return {};
        }
        Current().id = next_writer_++;
        Current().assigned = true;
      }
      auto writer = CreateWriter(Current().id);
      if (writer) Current().writer = writer;
      return writer;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++failures_;
      return {};
    }
#endif
  }
  std::optional<raw::Tag<SpaceType>> Resolve(Tag const& tag, Level level,
                                             std::string_view file,
                                             std::uint32_t line,
                                             std::string_view text) {
    std::uint32_t index;
    if constexpr (requires { Identity::ResolveIndex(file, line, tag); }) {
      index = Identity::ResolveIndex(file, line, tag);
    } else {
      std::uint64_t hash = 14695981039346656037ull;
      for (auto ch : file) {
        hash ^= static_cast<unsigned char>(ch == '\\' ? '/' : ch);
        hash *= 1099511628211ull;
      }
      for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (line >> shift) & 255;
        hash *= 1099511628211ull;
      }
      index = static_cast<std::uint32_t>(hash % 65535u) + 1;
    }
    std::lock_guard lock(sites_mutex_);
    if (index > raw::wire::kMaxTag) {
      sites_valid_ = false;
      ++failures_;
      return {};
    }
    auto found = sites_.find(index);
    if (found != sites_.end()) {
      auto const& existing = found->second;
      if (existing.file != file || existing.line != line ||
          existing.module != tag.module.id || existing.level != level.value_ ||
          existing.name != tag.name || existing.text != text) {
        sites_valid_ = false;
        ++failures_;
        return {};
      }
      return existing.Tag(index);
    }
    auto [entry, inserted] = sites_.emplace(
        index, Site{tag.module.id, level.value_, line, std::string{tag.name},
                    std::string{text}, std::string{file}});
    static_cast<void>(inserted);
    auto result = entry->second.Tag(index);
    return result;
  }
  SpaceType space_;
  // The shared_ptr atomic free functions also work on Apple libc++ versions
  // that do not implement C++20 atomic<shared_ptr<T>> yet.
  std::shared_ptr<raw::IBackend> backend_;
  std::shared_ptr<ITrap> legacy_trap_;
  std::shared_ptr<ITrap> env_bridge_;
  mutable std::shared_mutex binding_gate_;
  mutable std::mutex writers_mutex_, sites_mutex_;
  std::vector<std::shared_ptr<Writer>> writers_;
  std::map<std::uint32_t, Site> sites_;
  bool sites_valid_{true};
  std::uint32_t next_writer_{1};
  std::atomic<std::uint64_t> failures_{};
  static Cache& Current() {
    thread_local Cache current;
    return current;
  }
};

namespace raw_sink_internal {
template <typename Sink, TeleConfig Config>
class Event {
 public:
  template <typename... Args>
  Event(Sink& sink, Tag const& tag, Level level, std::string_view file,
        int line, FormatScheme format = collectors_internal::kEmptyFormat,
        Args&&... args) noexcept {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (raw::BackendCallbackActive()) {
        ++sink.failures_;
        return;
      }
      std::shared_lock binding(sink.binding_gate_);
      raw::BackendCallbackGuard operation;
      binding_gate_ = &sink.binding_gate_;
      auto options = raw::Restrict(sink.Runtime(), OptionsFor(Config));
      if (!(options.logs || options.console || options.count || options.timing))
        return;
      auto descriptor = sink.Resolve(
          tag, level, file, static_cast<std::uint32_t>(line), format.source);
      if (!descriptor) return;
      writer_ = sink.AcquireWriter();
      if (!writer_) return;
      std::string payload;
      if constexpr (Config.blob_logs && Config.logs_enabled) {
        if ((options.logs || options.console) &&
            (options.fields & raw::kPayload) && !format.source.empty())
          FormatTo(payload, format, std::forward<Args>(args)...);
      }
      raw::Data data{std::span<std::uint8_t const>{
          reinterpret_cast<std::uint8_t const*>(payload.data()),
          payload.size()}};
      std::lock_guard lock(writer_->mutex);
      writer_->clean = false;
      scope_.emplace(writer_->stream, *descriptor, options, data);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      ++sink.failures_;
    }
#endif
  }
  Event(Event const&) = delete;
  Event& operator=(Event const&) = delete;
  Event(Event&&) = delete;
  ~Event() noexcept {
    if (!writer_) return;
    std::shared_lock binding(*binding_gate_);
    raw::BackendCallbackGuard operation;
    std::lock_guard lock(writer_->mutex);
    writer_->clean = false;
    scope_.reset();
    if (writer_->retired.load() && writer_->stream.DrainMetrics())
      writer_->clean = true;
  }

 private:
  std::shared_mutex* binding_gate_{};
  std::shared_ptr<typename Sink::Writer> writer_;
  std::optional<raw::Scope<typename Sink::StreamType>> scope_;
};
}  // namespace raw_sink_internal

template <typename Identity, ConfigProvider CP, std::size_t Capacity,
          std::size_t MetricCapacity, TeleConfig Config>
struct Tele<RawTeleSink<Identity, CP, Capacity, MetricCapacity>, Config,
            std::enable_if_t<kIsAnyTele<Config>>>
    : raw_sink_internal::Event<
          RawTeleSink<Identity, CP, Capacity, MetricCapacity>, Config> {
  using raw_sink_internal::Event<
      RawTeleSink<Identity, CP, Capacity, MetricCapacity>, Config>::Event;
};
}  // namespace ae::tele
#endif  // AETHER_TELE_RAW_SINK_H_
