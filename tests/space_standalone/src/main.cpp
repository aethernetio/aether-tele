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

#include "workload.h"

#include <barrier>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "aether-tele/space.h"
#include "aether-tele/space/console.h"
#include "demo/application_space.h"
#include "demo/network_space.h"
#include "emit.h"

namespace fs = std::filesystem;
using ae::tele::space::StreamMarker;

namespace {

bool WriteBytes(fs::path const& path, std::vector<std::uint8_t> const& bytes) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<char const*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

std::vector<std::uint8_t> ReadBytes(fs::path const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void FillNetwork(demo::network::Storage& net) {
  demo::network::Stream main{net, StreamMarker::kMain};
  demo::network::Stream poller{net, StreamMarker::kPoller};
  demo::network::Stream network{net, StreamMarker::kNetwork};
  demo::network::Stream storage{net, StreamMarker::kStorage};
  demo::network::Stream worker{net, StreamMarker::kWorker0};
  for (int i = 0; i < 200; ++i) {
    demo::EmitPoll(poller);
    demo::network::Clock::advance(10);
  }
  for (int i = 0; i < 80; ++i) {
    demo::EmitPktRx(network, (i % 3 == 0) ? 80u : ((i % 3 == 1) ? 400u : 2000u));
    demo::network::Clock::advance(25);
  }
  for (int i = 0; i < 40; ++i) {
    demo::EmitPktTx(network, 120);
    demo::network::Clock::advance(30);
  }
  for (int i = 0; i < 8; ++i) {
    demo::EmitRequest(main, 500);
    demo::network::Clock::advance(40);
  }
  demo::EmitReconnect(storage);
  demo::network::Clock::advance(1000);
  demo::EmitNetError(worker, 5);
  demo::EmitByteCount(main);
  demo::EmitDisabledTap(main);
}

void FillApplication(demo::application::Storage& app) {
  demo::application::Stream main{app, StreamMarker::kMain};
  demo::application::Stream worker{app, StreamMarker::kWorker0};
  for (int i = 0; i < 120; ++i) {
    demo::EmitFrame(main);
    demo::application::Clock::advance(8);
  }
  for (int i = 0; i < 30; ++i) {
    demo::EmitJob(worker, static_cast<std::uint32_t>(i));
    demo::application::Clock::advance(12);
  }
  for (int i = 0; i < 10; ++i) {
    demo::EmitWork(main, 40);
    demo::application::Clock::advance(15);
  }
  demo::EmitUserEvent(main);
  demo::EmitFatal(worker, 9);
  demo::EmitJobsDone(main);
  demo::EmitDisabledAudit(main);
}

}  // namespace

int PhaseWrite(std::string const& dir) {
  fs::create_directories(dir);
  demo::network::Clock::reset(0);
  demo::application::Clock::reset(0);
  demo::network::UnixClock::set(1'700'000'000u);
  demo::application::UnixClock::set(1'700'000'050u);

  demo::network::Storage net;
  demo::application::Storage app;
  net.SetSequence(1);
  app.SetSequence(1);
  net.SetBaseUnix(demo::network::UnixClock::unix_seconds());
  app.SetBaseUnix(demo::application::UnixClock::unix_seconds());
  FillNetwork(net);
  FillApplication(app);

  auto net_blob = ae::tele::space::SerializeBlob(net);
  auto app_blob = ae::tele::space::SerializeBlob(app);
  WriteBytes(fs::path(dir) / "network.blob", net_blob);
  WriteBytes(fs::path(dir) / "application.blob", app_blob);
  {
    std::ofstream json(fs::path(dir) / "network.json");
    json << "{\"metrics\":[";
    bool first = true;
    auto dump = [&](char const* name, std::uint64_t count) {
      if (!first) {
        json << ",";
      }
      first = false;
      json << "{\"name\":\"" << name << "\",\"count\":" << count << "}";
    };
    auto metric = [&](std::uint32_t index) -> std::uint64_t {
      auto const* m = net.TryMetric(index);
      return m ? static_cast<std::uint64_t>(m->count) : 0;
    };
    dump("Poll", metric(demo::network::kPoll.index));
    dump("PktRx", metric(demo::network::kPktRx.index));
    dump("PktTx", metric(demo::network::kPktTx.index));
    dump("Request", metric(demo::network::kRequest.index));
    dump("Reconnect", metric(demo::network::kReconnect.index));
    dump("NetError", metric(demo::network::kNetError.index));
    dump("ByteCount", metric(demo::network::kByteCount.index));
    dump("DisabledTap", 0);
    json << "]}\n";
  }

  auto header = ae::tele::space::EncodeJournalHeader(1);
  auto b1 = ae::tele::space::EncodeBatch(net_blob);
  net.Clear();
  demo::network::Stream extra{net, StreamMarker::kMain};
  net.SetSequence(2);
  net.SetBaseUnix(1'700'000'010u);
  demo::EmitPoll(extra);
  auto net_blob2 = ae::tele::space::SerializeBlob(net);
  auto b2 = ae::tele::space::EncodeBatch(net_blob2);
  std::vector<std::uint8_t> journal = header;
  journal.insert(journal.end(), b1.begin(), b1.end());
  journal.insert(journal.end(), b2.begin(), b2.end());
  journal.insert(journal.end(), {'A', 'T', 'B', '1', 40, 0, 0, 0, 1, 2, 3});
  WriteBytes(fs::path(dir) / "network.journal", journal);
  std::cout << "write ok net=" << net_blob.size() << " app=" << app_blob.size()
            << " checksum_net=0x" << std::hex
            << demo::network::Config::kSchemaChecksum << " checksum_app=0x"
            << demo::application::Config::kSchemaChecksum << std::dec << '\n';
  return 0;
}

int PhaseResume(std::string const& dir) {
  auto bytes = ReadBytes(fs::path(dir) / "network.journal");
  auto journal = ae::tele::space::DecodeJournal(bytes);
  if (journal.sequence == 0 && journal.batches.empty()) {
    std::cerr << "resume: missing journal\n";
    return 1;
  }
  std::size_t committed = 0;
  for (auto const& batch : journal.batches) {
    if (batch.truncated || batch.corrupt) {
      continue;
    }
    auto decoded =
        ae::tele::space::DecodeBlob<demo::network::Config>(batch.blob);
    if (decoded.status != ae::tele::space::BlobStatus::kOk) {
      std::cerr << "resume: committed batch not decodable\n";
      return 1;
    }
    ++committed;
  }
  if (journal.truncated_tail == false) {
    std::cerr << "resume: expected truncated tail in write-phase journal\n";
    return 1;
  }
  demo::network::Clock::reset(0);
  demo::network::UnixClock::set(1'800'000'000u);
  demo::network::Storage net;
  net.SetSequence(journal.sequence + 1);
  net.SetBaseUnix(demo::network::UnixClock::unix_seconds());
  demo::network::Stream stream{net, StreamMarker::kMain};
  demo::EmitPoll(stream);
  demo::network::Clock::advance(77);
  demo::EmitPoll(stream);
  auto blob = ae::tele::space::SerializeBlob(net);
  WriteBytes(fs::path(dir) / "network.resume.blob", blob);
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  if (decoded.status != ae::tele::space::BlobStatus::kOk) {
    return 1;
  }
  if (decoded.data->sequence != journal.sequence + 1) {
    std::cerr << "resume: sequence not continued\n";
    return 1;
  }
  if (decoded.data->base_unix != 1'800'000'000u) {
    std::cerr << "resume: base_time reused previous process\n";
    return 1;
  }
  if (decoded.data->events.size() != 2 || decoded.data->events[0].delta != 0) {
    std::cerr << "resume: delta tied to previous process\n";
    return 1;
  }
  std::cout << "resume ok committed=" << committed
            << " truncated_tail=1 sequence=" << decoded.data->sequence << '\n';
  return 0;
}

int RunThreadTest() {
  demo::network::Clock::reset(0);
  demo::network::Storage net;
  std::barrier sync(5);
  auto worker = [&](StreamMarker marker, int polls, int packets) {
    demo::network::Stream stream{net, marker};
    sync.arrive_and_wait();
    for (int i = 0; i < polls; ++i) {
      demo::EmitPoll(stream);
      demo::network::Clock::advance(1);
    }
    for (int i = 0; i < packets; ++i) {
      demo::EmitPktRx(stream, 64);
      demo::network::Clock::advance(2);
    }
    sync.arrive_and_wait();
  };
  std::thread t0(worker, StreamMarker::kMain, 10, 0);
  std::thread t1(worker, StreamMarker::kPoller, 40, 0);
  std::thread t2(worker, StreamMarker::kNetwork, 0, 20);
  std::thread t3(worker, StreamMarker::kStorage, 2, 0);
  std::thread t4(worker, StreamMarker::kWorker0, 1, 3);
  t0.join();
  t1.join();
  t2.join();
  t3.join();
  t4.join();
  net.SetBaseUnix(42);
  auto blob = ae::tele::space::SerializeBlob(net);
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  if (decoded.status != ae::tele::space::BlobStatus::kOk) {
    std::cerr << "threads: decode failed\n";
    return 1;
  }
  if (decoded.data->streams.size() != 5) {
    std::cerr << "threads: expected 5 stream markers, got "
              << decoded.data->streams.size() << '\n';
    return 1;
  }
  auto const poll_count = net.TryMetric(demo::network::kPoll.index)->count;
  auto const rx_count = net.TryMetric(demo::network::kPktRx.index)->count;
  if (poll_count != 53 || rx_count != 23) {
    std::cerr << "threads: count mismatch poll=" << poll_count
              << " rx=" << rx_count << '\n';
    return 1;
  }
  for (auto const& stream : decoded.data->streams) {
    std::uint64_t prev = 0;
    for (auto const& event : stream.events) {
      if (event.rel_time < prev || event.rel_time > (1ull << 60)) {
        std::cerr << "threads: timeline broken on stream "
                  << static_cast<int>(stream.marker) << '\n';
        return 1;
      }
      prev = event.rel_time;
    }
  }
  std::cout << "threads ok streams=5 poll=" << poll_count
            << " rx=" << rx_count << '\n';
  return 0;
}

int RunStressTest() {
  demo::network::Clock::reset(0);
  demo::application::Clock::reset(0);
  demo::network::Storage net;
  demo::application::Storage app;
  auto net_fn = [&](StreamMarker marker, int n) {
    demo::network::Stream stream{net, marker};
    for (int i = 0; i < n; ++i) {
      demo::EmitPoll(stream);
      demo::network::Clock::advance(1);
    }
  };
  auto app_fn = [&](StreamMarker marker, int n) {
    demo::application::Stream stream{app, marker};
    for (int i = 0; i < n; ++i) {
      demo::EmitFrame(stream);
      demo::application::Clock::advance(1);
    }
  };
  std::thread n0(net_fn, StreamMarker::kMain, 2000);
  std::thread n1(net_fn, StreamMarker::kPoller, 3000);
  std::thread n2(net_fn, StreamMarker::kNetwork, 1000);
  std::thread a0(app_fn, StreamMarker::kMain, 2500);
  std::thread a1(app_fn, StreamMarker::kWorker0, 1500);
  n0.join();
  n1.join();
  n2.join();
  a0.join();
  a1.join();
  auto nb = ae::tele::space::SerializeBlob(net);
  auto ab = ae::tele::space::SerializeBlob(app);
  auto nd = ae::tele::space::DecodeBlob<demo::network::Config>(nb);
  auto ad = ae::tele::space::DecodeBlob<demo::application::Config>(ab);
  if (nd.status != ae::tele::space::BlobStatus::kOk ||
      ad.status != ae::tele::space::BlobStatus::kOk) {
    std::cerr << "stress: decode failed\n";
    return 1;
  }
  auto const poll_count = net.TryMetric(demo::network::kPoll.index)->count;
  auto const frame_count = app.TryMetric(demo::application::kFrame.index)->count;
  if (poll_count != 6000 || frame_count != 4000) {
    std::cerr << "stress: counts poll=" << poll_count
              << " frame=" << frame_count << '\n';
    return 1;
  }
  auto cross = ae::tele::space::DecodeBlob<demo::application::Config>(nb);
  if (cross.status != ae::tele::space::BlobStatus::kChecksumMismatch) {
    std::cerr << "stress: spaces leaked\n";
    return 1;
  }
  std::cout << "stress ok net_events=" << nd.data->event_count
            << " app_events=" << ad.data->event_count << '\n';
  return 0;
}

int PhaseDecode(std::string const& space, std::string const& blob_path,
                bool text) {
  auto blob = ReadBytes(blob_path);
  if (blob.empty()) {
    std::cerr << "failed to read blob\n";
    return 1;
  }
  auto print = [&](auto const& decoded) {
    if (decoded.status != ae::tele::space::BlobStatus::kOk || !decoded.data) {
      std::cerr << "decode failed status="
                << static_cast<int>(decoded.status) << " blob=0x" << std::hex
                << decoded.blob_checksum << " expected=0x"
                << decoded.expected_checksum << std::dec << '\n';
      return 1;
    }
    if (text) {
      for (auto const& event : decoded.data->events) {
        ae::tele::space::PrintDecodedEvent(event);
      }
      return 0;
    }
    std::cout << "{\n  \"space\": \"" << space << "\",\n  \"checksum\": "
              << decoded.data->checksum << ",\n  \"checksum_hex\": \"0x"
              << std::hex << decoded.data->checksum << std::dec
              << "\",\n  \"sequence\": " << decoded.data->sequence
              << ",\n  \"base_unix\": " << decoded.data->base_unix
              << ",\n  \"event_count\": " << decoded.data->event_count
              << ",\n  \"streams\": " << decoded.data->streams.size()
              << ",\n  \"library_version\": \"" << decoded.data->library_version
              << "\",\n  \"events\": [\n";
    for (std::size_t i = 0; i < decoded.data->events.size(); ++i) {
      auto const& e = decoded.data->events[i];
      std::cout << "    {\"rel_time\": " << e.rel_time << ", \"stream\": \""
                << ae::tele::space::MarkerName(e.stream) << "\", \"index\": "
                << e.index
#if defined(AE_TELE_SPACE_HOST)
                << ", \"name\": \"" << e.name << "\", \"module\": \""
                << e.module << "\", \"severity\": \""
                << ae::tele::space::SeverityName(e.severity) << "\""
#endif
                << "}";
      if (i + 1 != decoded.data->events.size()) {
        std::cout << ",";
      }
      std::cout << "\n";
    }
    std::cout << "  ],\n  \"metrics\": [\n";
    for (std::size_t i = 0; i < decoded.data->metrics.size(); ++i) {
      auto const& m = decoded.data->metrics[i];
      std::cout << "    {\"index\": " << m.index << ", \"count\": " << m.count
#if defined(AE_TELE_SPACE_HOST)
                << ", \"name\": \"" << m.name << "\""
#endif
                << "}";
      if (i + 1 != decoded.data->metrics.size()) {
        std::cout << ",";
      }
      std::cout << "\n";
    }
    std::cout << "  ]\n}\n";
    return 0;
  };
  if (space == "network") {
    return print(ae::tele::space::DecodeBlob<demo::network::Config>(blob));
  }
  if (space == "application") {
    return print(ae::tele::space::DecodeBlob<demo::application::Config>(blob));
  }
  std::cerr << "unknown space (blob has no namespace_id)\n";
  return 2;
}

int main(int argc, char** argv) {
  std::string phase = "unit";
  std::string dir = "space_out";
  std::string space;
  std::string blob;
  bool text = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--phase=", 0) == 0) {
      phase = arg.substr(8);
    } else if (arg.rfind("--dir=", 0) == 0) {
      dir = arg.substr(6);
    } else if (arg == "--space" && i + 1 < argc) {
      space = argv[++i];
    } else if (arg == "--text") {
      text = true;
    } else if (!arg.empty() && arg[0] != '-') {
      blob = arg;
    }
  }
  if (phase == "unit") {
    return RunUnitTests();
  }
  if (phase == "write") {
    return PhaseWrite(dir);
  }
  if (phase == "resume") {
    return PhaseResume(dir);
  }
  if (phase == "decode") {
    return PhaseDecode(space, blob, text);
  }
  if (phase == "threads") {
    return RunThreadTest();
  }
  if (phase == "stress") {
    return RunStressTest();
  }
  std::cerr << "usage: space_standalone --phase=unit|write|resume|decode|threads|stress\n";
  return 2;
}
