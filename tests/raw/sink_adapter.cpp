/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0.
 */
#include <unity.h>
#include <atomic>
#include <functional>
#include <sstream>
#include <thread>
#include "aether-tele/configs/all_enabled.h"
#include "aether-tele/defines.h"
#include "aether-tele/raw/sink.h"

AE_TELE_MODULE(module, 7, 1, 100);
AE_TAG_INDEXED(point, module, 0);
AE_TAG_INDEXED(second, module, 1);
namespace {
using ae::tele::raw::Decode;
using ae::tele::raw::DecodeStatus;
template <int N>
struct Identity {
  static constexpr std::string_view kName = "adapter";
  static std::uint32_t ResolveIndex(std::string_view, std::uint32_t,
                                    ae::tele::Tag const& tag) {
    return tag.index();
  }
};
template <int N>
using Sink = ae::tele::RawTeleSink<Identity<N>, ae::tele::AllEnabledConfig>;
struct Backend : ae::tele::raw::IBackend {
  ae::tele::raw::MemoryBackend journal;
  bool accept{true};
  std::function<void()> on_append;
  std::vector<std::vector<std::uint8_t>> attempts;
  bool Append(std::string_view name,
              std::span<std::uint8_t const> bytes) override {
    attempts.emplace_back(bytes.begin(), bytes.end());
    if (on_append) on_append();
    return accept && journal.Append(name, bytes);
  }
  bool RestoreMetrics(std::string_view name, std::uint32_t writer,
                      std::span<ae::tele::raw::MetricTotal> values,
                      std::size_t& count) override {
    return journal.RestoreMetrics(name, writer, values, count);
  }
  std::vector<std::uint8_t> Snapshot(std::string_view name) override {
    return journal.Snapshot(name);
  }
};
template <typename S>
void Emit() {
  AE_TELE_INFO_TO(S, point, "payload {} {}", 42, __func__);
}
void test_format_scope_and_map() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<1>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  TEST_ASSERT_TRUE(sink.BindWriter(7));
  Emit<Sink<1>>();
  TEST_ASSERT_TRUE(sink.DrainAll());
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(2, decoded.records.size());
  TEST_ASSERT_EQUAL_UINT(7, decoded.records[0].stream);
  TEST_ASSERT_TRUE(decoded.records[0].duration.has_value());
  auto payload = decoded.records[0].payload;
  TEST_ASSERT_EQUAL_STRING("payload 42 Emit",
                           std::string(payload.begin(), payload.end()).c_str());
  TEST_ASSERT_EQUAL_UINT(1, decoded.records[1].metric->count);
  TEST_ASSERT_EQUAL_UINT(1, decoded.records[1].metric->samples);
  std::ostringstream map;
  TEST_ASSERT_TRUE(sink.ExportMap(map, "test-build"));
  TEST_ASSERT_TRUE(map.str().find("utf8") != std::string::npos);
  TEST_ASSERT_TRUE(map.str().find("payload {} {}") != std::string::npos);
}
void test_runtime_mask_and_retry_bytes() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<2>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  sink.SetRuntime({true, false, true, true, 0});
  Emit<Sink<2>>();
  backend->accept = false;
  TEST_ASSERT_FALSE(sink.DrainCurrent());
  auto rejected = backend->attempts[0];
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).samples);
  Emit<Sink<2>>();
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).samples);
  backend->accept = true;
  TEST_ASSERT_TRUE(sink.DrainCurrent());
  TEST_ASSERT_TRUE(backend->attempts[1] == rejected);
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT(0, decoded.records[0].fields);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(2, metric.count);
  TEST_ASSERT_EQUAL_UINT(2, metric.samples);
}
void test_rebind_flushes_late_scope_duration_once() {
  auto early = std::make_shared<Backend>();
  auto persistent = std::make_shared<Backend>();
  auto& sink = Sink<3>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(early));
  {
    AE_TELE_INFO_TO(Sink<3>, point, "early");
    TEST_ASSERT_TRUE(sink.DrainAll());
    TEST_ASSERT_TRUE(persistent->Append("adapter", early->Snapshot("adapter")));
    TEST_ASSERT_TRUE(sink.SetBackend(persistent));
    {
      AE_TELE_INFO_TO(Sink<3>, second, "new");
    }
  }
  TEST_ASSERT_TRUE(sink.DrainAll());
  auto bytes = persistent->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(1, metric.count);
  TEST_ASSERT_EQUAL_UINT(1, metric.samples);
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).samples);
  auto size = bytes.size();
  TEST_ASSERT_TRUE(sink.DrainAll());
  TEST_ASSERT_EQUAL_UINT(size, persistent->Snapshot("adapter").size());
}
void test_explicit_live_writer_id_is_unique() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<4>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  TEST_ASSERT_TRUE(sink.BindWriter(17));
  std::atomic<bool> rejected{};
  std::thread other([&] { rejected = !sink.BindWriter(17); });
  other.join();
  TEST_ASSERT_TRUE(rejected.load());
  Emit<Sink<4>>();
  TEST_ASSERT_TRUE(sink.DrainAll());
}
void test_two_app_like_backend_cycles_preserve_disjoint_statistics() {
  auto& sink = Sink<6>::Instance();
  std::vector<std::uint8_t> saved;
  for (unsigned boot = 0; boot != 3; ++boot) {
    auto early = std::make_shared<Backend>();
    TEST_ASSERT_TRUE(sink.SetBackend(early));
    sink.SetRuntime({});
    TEST_ASSERT_TRUE(sink.BindWriter(0));
    Emit<Sink<6>>();
    TEST_ASSERT_TRUE(sink.DrainAll());
    auto persistent = std::make_shared<Backend>();
    TEST_ASSERT_TRUE(persistent->Append("adapter", saved));
    TEST_ASSERT_TRUE(persistent->Append("adapter", early->Snapshot("adapter")));
    TEST_ASSERT_TRUE(sink.SetBackend(persistent));
    Emit<Sink<6>>();
    TEST_ASSERT_TRUE(sink.DrainAll());
    saved = persistent->Snapshot("adapter");
    auto decoded = Decode(saved);
    TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
    auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
    TEST_ASSERT_EQUAL_UINT(2 * (boot + 1), metric.count);
    TEST_ASSERT_EQUAL_UINT(2 * (boot + 1), metric.samples);
    TEST_ASSERT_EQUAL_UINT(2 * (boot + 1), sink.MetricFor(1).count);
    auto size = saved.size();
    TEST_ASSERT_TRUE(sink.DrainAll());
    TEST_ASSERT_EQUAL_UINT(size, persistent->Snapshot("adapter").size());
  }
}
void test_concurrent_checkpoint_and_owner_completion_keep_every_delta() {
  auto backend = std::make_shared<ae::tele::raw::MemoryBackend>();
  auto& sink = Sink<7>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  TEST_ASSERT_TRUE(sink.BindWriter(0));
  std::array<std::thread, 4> workers;
  std::atomic<unsigned> done{};
  std::atomic<bool> failed{};
  for (unsigned id = 0; id < workers.size(); ++id)
    workers[id] = std::thread([&, id] {
      if (!sink.BindWriter(100 + id)) failed = true;
      for (unsigned i = 0; i != 100; ++i) Emit<Sink<7>>();
      ++done;
    });
  std::thread checkpoint([&] {
    while (done.load() != workers.size()) {
      if (!sink.DrainAll()) failed = true;
      std::this_thread::yield();
    }
  });
  for (auto& worker : workers) worker.join();
  checkpoint.join();
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.DrainAll());
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(400, metric.count);
  TEST_ASSERT_EQUAL_UINT(400, metric.samples);
  auto rows = std::count_if(
      decoded.records.begin(), decoded.records.end(),
      [](auto const& row) { return row.kind == ae::tele::raw::Kind::kLog; });
  TEST_ASSERT_EQUAL_UINT(400, rows);
}
void test_ended_writer_id_restores_pending_predecessor_before_reuse() {
  auto backend = std::make_shared<ae::tele::raw::MemoryBackend>();
  auto& sink = Sink<8>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  std::atomic<bool> failed{};
  for (unsigned i = 0; i != 3; ++i) {
    std::thread worker([&] {
      if (!sink.BindWriter(17)) failed = true;
      Emit<Sink<8>>();
    });
    worker.join();
  }
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.DrainAll());
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(3, metric.count);
  TEST_ASSERT_EQUAL_UINT(3, metric.samples);
}
void test_backend_reentry_rejected_across_writers_and_namespaces() {
  auto backend = std::make_shared<Backend>();
  auto& source = Sink<9>::Instance();
  auto& other_space = Sink<10>::Instance();
  TEST_ASSERT_TRUE(source.SetBackend(backend));
  TEST_ASSERT_TRUE(other_space.SetBackend(backend));
  TEST_ASSERT_TRUE(source.BindWriter(0));
  Emit<Sink<9>>();
  std::atomic<bool> bound{};
  std::thread worker([&] {
    bound = source.BindWriter(17);
    Emit<Sink<9>>();
  });
  worker.join();
  TEST_ASSERT_TRUE(bound.load());
  bool rejected = true;
  backend->on_append = [&] {
    Emit<Sink<9>>();  // main TLS writer differs when checkpoint drains worker17
    Emit<Sink<10>>();  // a different namespace sharing this backend
    rejected = !source.DrainCurrent() && rejected;
    rejected = !source.DrainAll() && rejected;
    rejected = !other_space.DrainAll() && rejected;
    rejected = !source.BindWriter(0) && rejected;
    rejected = !source.SetBackend(backend) && rejected;
  };
  TEST_ASSERT_TRUE(source.DrainAll());
  TEST_ASSERT_TRUE(rejected);
  backend->on_append = {};
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(2, metric.count);
  TEST_ASSERT_EQUAL_UINT(2, metric.samples);
  TEST_ASSERT_TRUE(other_space.failures() != 0);
}
template <typename S>
void EmitWhilePaused(bool pause, std::atomic<unsigned>& entered,
                     std::atomic<bool>& release) {
  AE_TELE_INFO_TO(S, point, "concurrent");
  if (pause) {
    ++entered;
    while (!release.load()) std::this_thread::yield();
  }
}
void test_backend_migration_is_atomic_with_concurrent_open_scopes() {
  auto backend = std::make_shared<ae::tele::raw::MemoryBackend>();
  auto& sink = Sink<11>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  TEST_ASSERT_TRUE(sink.BindWriter(0));
  std::atomic<unsigned> entered{};
  std::atomic<bool> release{}, failed{};
  std::array<std::thread, 2> workers;
  for (unsigned id = 0; id < workers.size(); ++id)
    workers[id] = std::thread([&, id] {
      if (!sink.BindWriter(17 + id)) failed = true;
      for (unsigned i = 0; i != 100; ++i)
        EmitWhilePaused<Sink<11>>(i == 0, entered, release);
    });
  while (entered.load() != workers.size()) std::this_thread::yield();
  for (unsigned bind = 0; bind != 3; ++bind) {
    auto next = std::make_shared<ae::tele::raw::MemoryBackend>();
    if (!sink.SetBackend(next, [&](auto const& old) {
          return next->Append("adapter", old->Snapshot("adapter"));
        }))
      failed = true;
    backend = next;
    release = true;
    std::this_thread::yield();
  }
  for (auto& worker : workers) worker.join();
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.DrainAll());
  auto bytes = backend->Snapshot("adapter");
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  auto metric = ae::tele::raw::RestoreMetric(decoded.records, 1);
  TEST_ASSERT_EQUAL_UINT(200, metric.count);
  TEST_ASSERT_EQUAL_UINT(200, metric.samples);
  auto rows = std::count_if(
      decoded.records.begin(), decoded.records.end(),
      [](auto const& row) { return row.kind == ae::tele::raw::Kind::kLog; });
  TEST_ASSERT_EQUAL_UINT(200, rows);
  auto rejected = std::make_shared<ae::tele::raw::MemoryBackend>();
  TEST_ASSERT_FALSE(
      sink.SetBackend(rejected, [](auto const&) { return false; }));
  TEST_ASSERT_TRUE(sink.GetBackend() == backend);
  TEST_ASSERT_TRUE(rejected->Snapshot("adapter").empty());
}
void test_canonical_query_saturates_without_adding_restored_totals_twice() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<12>::Instance();
  auto maximum = std::numeric_limits<std::uint64_t>::max();
  std::array<std::uint8_t, 128> bytes{};
  auto cursor = ae::tele::raw::wire::kBlockPrefixReserve;
  bytes[cursor++] =
      ae::tele::raw::wire::Control(ae::tele::raw::wire::kMetricBlock, 0, true);
  TEST_ASSERT_TRUE(ae::tele::raw::wire::Write<ae::tele::raw::wire::StreamCodec>(
      0, bytes, cursor));
  TEST_ASSERT_TRUE(ae::tele::raw::wire::WriteU64(0, bytes, cursor));
  TEST_ASSERT_TRUE(ae::tele::raw::wire::Write<ae::tele::raw::wire::TagCodec>(
      1, bytes, cursor));
  for (auto value :
       {maximum, maximum, maximum, std::uint64_t{1}, std::uint64_t{2}})
    TEST_ASSERT_TRUE(ae::tele::raw::wire::WriteU64(value, bytes, cursor));
  TEST_ASSERT_TRUE(backend->Append(
      "adapter", ae::tele::raw::wire::FinishBlock(bytes, cursor)));
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  TEST_ASSERT_TRUE(sink.BindWriter(0));
  Emit<Sink<12>>();
  auto before = sink.MetricFor(1);
  TEST_ASSERT_TRUE(before.count == maximum);
  TEST_ASSERT_TRUE(before.samples == maximum);
  TEST_ASSERT_TRUE(before.sum == maximum);
  backend->accept = false;
  TEST_ASSERT_FALSE(sink.DrainCurrent());
  auto retry = sink.MetricFor(1);
  TEST_ASSERT_TRUE(retry.count == maximum && retry.samples == maximum &&
                   retry.sum == maximum);
  backend->accept = true;
  TEST_ASSERT_TRUE(sink.DrainCurrent());
  auto after = sink.MetricFor(1);
  TEST_ASSERT_TRUE(after.count == maximum && after.samples == maximum &&
                   after.sum == maximum);
}
void test_query_includes_frozen_metric_retry_and_later_delta_once() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<13>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  sink.SetRuntime({false, false, true, true, 0});
  Emit<Sink<13>>();
  backend->accept = false;
  TEST_ASSERT_FALSE(sink.DrainCurrent());
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(1, sink.MetricFor(1).samples);
  Emit<Sink<13>>();
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).samples);
  backend->accept = true;
  TEST_ASSERT_TRUE(sink.DrainCurrent());
  TEST_ASSERT_TRUE(backend->attempts[0] == backend->attempts[1]);
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).count);
  TEST_ASSERT_EQUAL_UINT(2, sink.MetricFor(1).samples);
}
void test_environment_bridge_owning_snapshot() {
  auto backend = std::make_shared<Backend>();
  auto& sink = Sink<5>::Instance();
  TEST_ASSERT_TRUE(sink.SetBackend(backend));
  auto trap = sink.GetTrap();
  TEST_ASSERT_NOT_NULL(trap.get());
  trap->WriteEnvData({});
}
}  // namespace
void setUp() {}
void tearDown() {}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_format_scope_and_map);
  RUN_TEST(test_runtime_mask_and_retry_bytes);
  RUN_TEST(test_rebind_flushes_late_scope_duration_once);
  RUN_TEST(test_explicit_live_writer_id_is_unique);
  RUN_TEST(test_environment_bridge_owning_snapshot);
  RUN_TEST(test_two_app_like_backend_cycles_preserve_disjoint_statistics);
  RUN_TEST(test_concurrent_checkpoint_and_owner_completion_keep_every_delta);
  RUN_TEST(test_ended_writer_id_restores_pending_predecessor_before_reuse);
  RUN_TEST(test_backend_reentry_rejected_across_writers_and_namespaces);
  RUN_TEST(test_backend_migration_is_atomic_with_concurrent_open_scopes);
  RUN_TEST(test_canonical_query_saturates_without_adding_restored_totals_twice);
  RUN_TEST(test_query_includes_frozen_metric_retry_and_later_delta_once);
  return UNITY_END();
}
