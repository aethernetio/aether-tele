#include "legacy_spaces.h"
#include <unity.h>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <cstdio>

// This was valid in the original public API. A capture-default lambda at this
// location is ill-formed, so keep it as a compilation regression as well.
AE_TELE_(global_checkpoint, routing::tag, ::ae::tele::Level::kInfo, "global checkpoint");
namespace {
class Trap final : public ae::tele::ITrap, public ae::tele::ILogLine {
 public:
  unsigned count{}, durations{};
  std::string message;
  void AddInvoke(ae::tele::Tag const&, std::uint32_t n) override { count += n; }
  void AddInvokeDuration(ae::tele::Tag const&, ae::tele::Duration) override { ++durations; }
  void LogLine(ae::tele::Tag const&, ae::tele::ILogCollector& c) override { c.WriteLine(*this); }
  void WriteEnvData(ae::tele::EnvData const&) override {}
  void InvokeTime(ae::tele::TimePoint) override {}
  void WriteLevel(ae::tele::Level) override {}
  void WriteModule(ae::tele::Module const&) override {}
  void Location(std::string_view, std::uint32_t) override {}
  void TagName(std::string_view) override {}
  void Blob(std::span<std::uint8_t const> b) override { message.assign(reinterpret_cast<char const*>(b.data()), b.size()); }
};
void test_routes_survive_header_order_and_shared_config() {
  auto a = std::make_shared<Trap>(); auto b = std::make_shared<Trap>();
  routing::SinkA::Instance().SetTrap(a); routing::SinkB::Instance().SetTrap(b);
  routing::OrderAB(); routing::OrderBA();
  TEST_ASSERT_EQUAL_UINT(2, a->count); TEST_ASSERT_EQUAL_UINT(2, b->count);
  TEST_ASSERT_EQUAL_UINT(2, a->durations); TEST_ASSERT_EQUAL_UINT(2, b->durations);
  TEST_ASSERT_EQUAL_STRING("value 42", a->message.c_str());
  TEST_ASSERT_EQUAL_STRING("value 7", b->message.c_str());
  routing::SinkA::Instance().SetTrap(nullptr);
  TEST_ASSERT_NOT_NULL(routing::SinkB::Instance().trap().get());
  routing::SinkB::Instance().SetTrap(nullptr);
}
void test_original_legacy_argument_evaluation() {
  int side_effect = 0;
  { AE_TELE_INFO_TO(routing::OffSink, routing::tag, "{}", ++side_effect); }
  auto trap = std::make_shared<Trap>(); routing::NoBlobSink::Instance().SetTrap(trap);
  { AE_TELE_INFO_TO(routing::NoBlobSink, routing::tag, "{}", ++side_effect); }
  TEST_ASSERT_EQUAL_INT(2, side_effect); TEST_ASSERT_EQUAL_UINT(1, trap->count);
  TEST_ASSERT_EQUAL_UINT(1, trap->durations); TEST_ASSERT_TRUE(trap->message.empty());
  routing::NoBlobSink::Instance().SetTrap(nullptr);
}

void test_formatting_keeps_caller_function_and_names() {
  auto trap = std::make_shared<Trap>();
  routing::SinkA::Instance().SetTrap(trap);
  std::string expected = __func__;
  int ae_tele_config = 123;
  unsigned durations_inside = 0;
  {
    AE_TELE_INFO_TO(routing::SinkA, routing::tag, "{} {}", __func__, ae_tele_config);
    // The RAII timer must live until this enclosing scope ends.
    durations_inside = trap->durations;
  }
  TEST_ASSERT_EQUAL_UINT(0, durations_inside);
  TEST_ASSERT_EQUAL_STRING((expected + " 123").c_str(), trap->message.c_str());
  TEST_ASSERT_EQUAL_UINT(1, trap->durations);
  routing::SinkA::Instance().SetTrap(nullptr);
}

class AtomicTrap final : public ae::tele::ITrap {
 public:
  std::atomic<unsigned> count{0}, durations{0}, logs{0};
  void AddInvoke(ae::tele::Tag const&, std::uint32_t n) override { count += n; }
  void AddInvokeDuration(ae::tele::Tag const&, ae::tele::Duration) override { ++durations; }
  void LogLine(ae::tele::Tag const&, ae::tele::ILogCollector&) override { ++logs; }
  void WriteEnvData(ae::tele::EnvData const&) override {}
};

struct AlternatingSink {
  static inline std::shared_ptr<AtomicTrap> first, second;
  static inline unsigned reads{};
  template <auto, auto>
  static consteval ae::tele::TeleConfig GetTeleConfig() { return {}; }
  static AlternatingSink& Instance() { static AlternatingSink sink; return sink; }
  std::shared_ptr<ae::tele::ITrap> GetTrap() const {
    return ++reads == 1 ? first : second;
  }
};
void test_one_snapshot_for_complete_event() {
  AlternatingSink::first = std::make_shared<AtomicTrap>();
  AlternatingSink::second = std::make_shared<AtomicTrap>();
  AlternatingSink::reads = 0;
  { AE_TELE_INFO_TO(AlternatingSink, routing::tag); }
  TEST_ASSERT_EQUAL_UINT(1, AlternatingSink::reads);
  TEST_ASSERT_EQUAL_UINT(1, AlternatingSink::first->count.load());
  TEST_ASSERT_EQUAL_UINT(1, AlternatingSink::first->durations.load());
  TEST_ASSERT_EQUAL_UINT(1, AlternatingSink::first->logs.load());
  TEST_ASSERT_EQUAL_UINT(0, AlternatingSink::second->count.load());
  AlternatingSink::first.reset(); AlternatingSink::second.reset();
}

struct OldCustomSink {
  std::shared_ptr<ae::tele::ITrap> trap_;
  template <auto, auto>
  static consteval ae::tele::TeleConfig GetTeleConfig() { return {}; }
  static OldCustomSink& Instance() { static OldCustomSink sink; return sink; }
  // The original collector accepted non-const custom sinks and did not require
  // a new accessor. An unrelated method must not override that contract.
  std::shared_ptr<ae::tele::ITrap>& trap() { return trap_; }
  int GetTrap() { return 123; }
};
void test_original_custom_sink_contract() {
  auto trap = std::make_shared<Trap>();
  OldCustomSink::Instance().trap() = trap;
  { AE_TELE_INFO_TO(OldCustomSink, routing::tag, "custom {}", 1); }
  TEST_ASSERT_EQUAL_UINT(1, trap->count);
  TEST_ASSERT_EQUAL_UINT(1, trap->durations);
  TEST_ASSERT_EQUAL_STRING("custom 1", trap->message.c_str());
  OldCustomSink::Instance().trap().reset();
}

void test_active_scope_keeps_replaced_trap_alive() {
  auto first = std::make_shared<Trap>(); auto second = std::make_shared<Trap>();
  std::weak_ptr<Trap> weak = first;
  routing::SinkA::Instance().SetTrap(first);
  bool alive_inside = false;
  unsigned replacement_durations = 0;
  {
    AE_TELE_INFO_TO(routing::SinkA, routing::tag);
    routing::SinkA::Instance().SetTrap(second);
    first.reset();
    alive_inside = !weak.expired();
    replacement_durations = second->durations;
  }
  TEST_ASSERT_TRUE(alive_inside);
  TEST_ASSERT_EQUAL_UINT(0, replacement_durations);
  TEST_ASSERT_TRUE(weak.expired());
  TEST_ASSERT_EQUAL_UINT(0, second->durations);
  routing::SinkA::Instance().SetTrap(nullptr);
}

void test_concurrent_trap_replacement() {
  auto first = std::make_shared<AtomicTrap>(); auto second = std::make_shared<AtomicTrap>();
  routing::SinkA::Instance().SetTrap(first);
  std::atomic<bool> start{false};
  std::thread publisher([&] {
    while (!start.load(std::memory_order_acquire)) {}
    for (unsigned i = 0; i != 10000; ++i) {
      routing::SinkA::Instance().SetTrap((i & 1) ? first : second);
      if (i % 7 == 0) routing::SinkA::Instance().SetTrap(nullptr);
    }
  });
  std::thread writer([&] {
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i != 10000; ++i) {
      AE_TELE_INFO_TO(routing::SinkA, routing::tag);
    }
  });
  std::thread reader([&] {
    while (!start.load(std::memory_order_acquire)) {}
    for (unsigned i = 0; i != 10000; ++i) {
      auto owning_snapshot = routing::SinkA::Instance().GetTrap();
      (void)owning_snapshot;
    }
  });
  publisher.join(); writer.join(); reader.join();
  for (auto const& trap : {first, second}) {
    TEST_ASSERT_EQUAL_UINT(trap->count.load(), trap->logs.load());
    TEST_ASSERT_EQUAL_UINT(trap->count.load(), trap->durations.load());
  }
  routing::SinkA::Instance().SetTrap(nullptr);
}
}
void setUp() {}
void tearDown() {}
int main() { std::setbuf(stdout, nullptr); UNITY_BEGIN(); RUN_TEST(test_routes_survive_header_order_and_shared_config);
  RUN_TEST(test_original_legacy_argument_evaluation);
  RUN_TEST(test_formatting_keeps_caller_function_and_names);
  RUN_TEST(test_one_snapshot_for_complete_event);
  RUN_TEST(test_original_custom_sink_contract);
  RUN_TEST(test_active_scope_keeps_replaced_trap_alive);
  RUN_TEST(test_concurrent_trap_replacement);
  return UNITY_END(); }
