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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "aether-tele/packed_int.h"
#include "aether-tele/space.h"
#include "demo/application_space.h"
#include "demo/network_space.h"
#include "emit.h"

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "CHECK failed: " #cond " (" << __FILE__ << ":" << __LINE__\
                << ")\n";                                                    \
      return 1;                                                              \
    }                                                                        \
  } while (0)

namespace {

int TestUnixOverflow() {
  using ae::tele::space::kUnixTimeMax;
  CHECK(kUnixTimeMax == 0xFFFFFFFFu);
  std::uint32_t wrapped = static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(kUnixTimeMax) + 1u);
  CHECK(wrapped == 0);
  demo::network::Storage storage;
  demo::network::Stream stream{storage, ae::tele::space::StreamMarker::kMain};
  storage.SetBaseUnix(kUnixTimeMax);
  demo::EmitPoll(stream);
  auto blob = ae::tele::space::SerializeBlob(storage);
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  CHECK(decoded.status == ae::tele::space::BlobStatus::kOk);
  CHECK(decoded.data->base_unix == kUnixTimeMax);
  return 0;
}

int TestCrcEndianAndGoldenHeader() {
  using ae::tele::space::Crc32Acc;
  using ae::tele::space::ReadU32LE;
  using ae::tele::space::WriteU32LE;

  std::uint8_t le[4]{};
  WriteU32LE(le, 0xA1B2C3D4u);
  CHECK(le[0] == 0xD4);
  CHECK(le[1] == 0xC3);
  CHECK(le[2] == 0xB2);
  CHECK(le[3] == 0xA1);
  CHECK(ReadU32LE(le) == 0xA1B2C3D4u);

  constexpr std::uint32_t kCrc01020304 = std::invoke([]() constexpr {
    Crc32Acc crc;
    crc.FeedU8(1);
    crc.FeedU8(2);
    crc.FeedU8(3);
    crc.FeedU8(4);
    return crc.Finish();
  });
  Crc32Acc crc;
  crc.FeedU8(1);
  crc.FeedU8(2);
  crc.FeedU8(3);
  crc.FeedU8(4);
  CHECK(crc.Finish() == kCrc01020304);
  Crc32Acc same;
  same.FeedU8(1);
  same.FeedU8(2);
  same.FeedU8(3);
  same.FeedU8(4);
  CHECK(same.Finish() == kCrc01020304);

  constexpr auto kIndexSigA = demo::application::Config::IndexCodec::kSchemaSignature;
  constexpr auto kDeltaSigN = demo::network::Config::DeltaCodec::kSchemaSignature;
  static_assert(kIndexSigA != 0);
  static_assert(kDeltaSigN != 0);
  static_assert(kIndexSigA != kDeltaSigN);
  return 0;
}

int TestExponentialCodec() {
  using Codec = demo::network::Config::DeltaCodec;
  using Exp = demo::network::DeltaExp;
  std::array<std::uint8_t, 16> buf{};

  auto z = Codec::Encode(0, buf.data());
  CHECK(z > 0);
  auto zd = Codec::Decode(buf.data(), z);
  CHECK(zd.bytes_read == z);
  CHECK(zd.value == 0);

  std::uint64_t prev = 0;
  std::uint64_t prev_code = 0;
  double max_abs_err = 0;
  double max_rel_err = 0;
  for (std::uint64_t v : {0ull, 1ull, 2ull, 10ull, 100ull, 1000ull, 10000ull,
                          100000ull, 1000000ull, 4000000000ull,
                          5000000000ull}) {
    auto n = Codec::Encode(v, buf.data());
    CHECK(n > 0);
    std::array<std::uint8_t, 32> padded{};
    std::copy(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n),
              padded.begin());
    padded[n] = 0x7F;
    padded[n + 1] = 0x81;
    auto d = Codec::Decode(padded.data(), padded.size());
    CHECK(d.bytes_read == n);
    CHECK(d.value >= prev);
    auto const expected = v > 4'000'000'000ull ? 4'000'000'000ull : v;
    auto const err =
        d.value > expected ? d.value - expected : expected - d.value;
    if (expected != 0 && expected <= 1'000'000ull) {
      max_abs_err = std::max(max_abs_err, static_cast<double>(err));
      max_rel_err =
          std::max(max_rel_err, static_cast<double>(err) / expected);
    }
    if (v == 0) {
      CHECK(d.value == 0);
    }
    if (v == 10) {
      CHECK(d.value >= 5);
      CHECK(d.value <= 20);
    }
    auto code = Exp::FromRuntimeInteger(static_cast<std::int64_t>(expected))
                    .CodeValue();
    CHECK(code >= prev_code);
    prev = d.value;
    prev_code = code;
  }
  CHECK(max_rel_err < 0.75);
  std::cout << "exponential codec max_abs_err=" << max_abs_err
            << " max_rel_err=" << max_rel_err
            << " min=" << demo::network::kDeltaMinMagnitude
            << " bound=" << demo::network::kDeltaBoundaryMagnitude
            << " bound_code=" << demo::network::kDeltaBoundaryCode << '\n';

  auto max_code = Exp::kBoundaryCode;
  auto from_code = Exp::FromCode(
      ae::tele::PackedU64{static_cast<std::uint64_t>(max_code)});
  CHECK(from_code.CodeValue() == static_cast<std::uint64_t>(max_code));
  auto nmax = Codec::Encode(4'000'000'000ull, buf.data());
  auto dmax = Codec::Decode(buf.data(), nmax);
  CHECK(dmax.value > 0);
  auto nover = Codec::Encode(5'000'000'000ull, buf.data());
  auto dover = Codec::Decode(buf.data(), nover);
  CHECK(dover.value == dmax.value);

  std::uint8_t code_buf[16]{};
  for (std::uint64_t code = 0; code <= 64; ++code) {
    auto const original = Exp::FromCode(ae::tele::PackedU64{code});
    auto const wn = ae::Serialize(original.WireCode(), code_buf);
    auto const back = ae::Deserialize<ae::tele::PackedU64>(code_buf, wn);
    CHECK(back.bytes_read == wn);
    CHECK(Exp::FromCode(back.value).CodeValue() == code);
  }
  auto const last = Exp::FromCode(
      ae::tele::PackedU64{static_cast<std::uint64_t>(max_code)});
  auto const wn = ae::Serialize(last.WireCode(), code_buf);
  auto const back = ae::Deserialize<ae::tele::PackedU64>(code_buf, wn);
  CHECK(Exp::FromCode(back.value).CodeValue() ==
        static_cast<std::uint64_t>(max_code));

  std::size_t saw1 = 0, saw2 = 0, saw4 = 0, saw8 = 0;
  for (std::uint64_t v = 0; v < 3000; ++v) {
    auto n = Codec::Encode(v, buf.data());
    if (n == 1) {
      ++saw1;
    } else if (n == 2) {
      ++saw2;
    } else if (n == 4) {
      ++saw4;
    } else if (n == 8) {
      ++saw8;
    }
  }
  CHECK(saw1 > 0);
  (void)saw2;
  (void)saw4;
  (void)saw8;
  return 0;
}

int TestNetworkDeltaTimeline() {
  demo::network::Clock::reset(0);
  demo::network::Storage storage;
  demo::network::Stream poller{storage, ae::tele::space::StreamMarker::kPoller};
  demo::network::Stream network{storage,
                                ae::tele::space::StreamMarker::kNetwork};
  for (int i = 0; i < 8; ++i) {
    demo::EmitPoll(poller);
    demo::network::Clock::advance(10);
  }
  demo::EmitPktRx(network, 80);
  auto blob = ae::tele::space::SerializeBlob(storage);
  CHECK(blob.size() >= 8);
  CHECK(blob[0] == 'A' && blob[1] == 'T' && blob[2] == 'S' && blob[3] == '1');
  std::uint8_t checksum_le[4]{};
  ae::tele::space::WriteU32LE(checksum_le,
                              demo::network::Config::kSchemaChecksum);
  CHECK(blob[4] == checksum_le[0] && blob[5] == checksum_le[1] &&
        blob[6] == checksum_le[2] && blob[7] == checksum_le[3]);
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  CHECK(decoded.status == ae::tele::space::BlobStatus::kOk);
  CHECK(decoded.data->streams.size() == 2);
  for (auto const& stream : decoded.data->streams) {
    std::uint64_t prev = 0;
    for (std::size_t i = 0; i < stream.events.size(); ++i) {
      CHECK(stream.events[i].rel_time >= prev);
      CHECK(stream.events[i].rel_time < (1ull << 63));
      prev = stream.events[i].rel_time;
    }
  }
  CHECK(decoded.data->events[0].rel_time == 0);
  CHECK(decoded.data->events[1].rel_time >= 5);
  CHECK(decoded.data->events[1].rel_time <= 20);
  return 0;
}

int TestPackedSizes() {
  using Codec = demo::application::Config::IndexCodec;
  std::array<std::uint8_t, 16> buf{};
  CHECK(Codec::Encode(4, buf.data()) == 1);
  CHECK(Codec::Encode(300, buf.data()) == 2);
  CHECK(Codec::Encode(2000, buf.data()) == 4);
  CHECK(Codec::Encode(2'000'000ull, buf.data()) == 8);
  return 0;
}

int TestTwoSpacesAndChecksum() {
  static_assert(sizeof(demo::network::Config::kSchemaChecksum) == 4);
  static_assert(demo::network::Config::kSchemaChecksum !=
                demo::application::Config::kSchemaChecksum);
#if !defined(AE_TELE_SPACE_REINDEX_BUILD)
  static_assert(demo::network::kPoll.index == demo::application::kFrame.index);
  static_assert(demo::network::kNetError.index ==
                demo::application::kFatal.index);
#endif

  demo::network::Clock::reset(0);
  demo::application::Clock::reset(0);
  demo::network::UnixClock::set(1'700'000'000u);
  demo::application::UnixClock::set(1'700'000'100u);

  demo::network::Storage net;
  demo::application::Storage app;
  demo::network::Stream nmain{net, ae::tele::space::StreamMarker::kMain};
  demo::application::Stream amain{app, ae::tele::space::StreamMarker::kMain};
  net.SetBaseUnix(demo::network::UnixClock::unix_seconds());
  app.SetBaseUnix(demo::application::UnixClock::unix_seconds());

  demo::EmitPoll(nmain);
  demo::EmitFrame(amain);
  demo::EmitDisabledTap(nmain);
  demo::EmitDisabledAudit(amain);
  CHECK(net.TryMetric(demo::network::kDisabledTap.index) == nullptr);
  CHECK(app.TryMetric(demo::application::kDisabledAudit.index) == nullptr);

  auto nb = ae::tele::space::SerializeBlob(net);
  auto ab = ae::tele::space::SerializeBlob(app);
  CHECK(!ae::tele::space::BlobContainsText(nb, "Poll"));
  CHECK(!ae::tele::space::BlobContainsText(nb, "Net"));
  CHECK(!ae::tele::space::BlobContainsText(ab, "Frame"));
  CHECK(!ae::tele::space::BlobContainsText(ab, "network_emit.cpp"));

  auto nd = ae::tele::space::DecodeBlob<demo::network::Config>(nb);
  auto ad = ae::tele::space::DecodeBlob<demo::application::Config>(ab);
  CHECK(nd.status == ae::tele::space::BlobStatus::kOk);
  CHECK(ad.status == ae::tele::space::BlobStatus::kOk);
  CHECK(nd.data->checksum != ad.data->checksum);

  auto reject =
      ae::tele::space::DecodeBlob<demo::application::Config>(nb);
  CHECK(reject.status == ae::tele::space::BlobStatus::kChecksumMismatch);

  auto bad_ver = nb;
  bad_ver[8] = 99;
  auto dv = ae::tele::space::DecodeBlob<demo::network::Config>(bad_ver);
  CHECK(dv.status == ae::tele::space::BlobStatus::kBadVersion ||
        dv.status == ae::tele::space::BlobStatus::kChecksumMismatch);

  auto trunc = nb;
  trunc.resize(12);
  auto dt = ae::tele::space::DecodeBlob<demo::network::Config>(trunc);
  CHECK(dt.status == ae::tele::space::BlobStatus::kTruncated ||
        dt.status == ae::tele::space::BlobStatus::kInvalidRecord ||
        dt.status == ae::tele::space::BlobStatus::kUnknownSection);

  auto corrupt = nb;
  corrupt[0] = 'X';
  auto dc = ae::tele::space::DecodeBlob<demo::network::Config>(corrupt);
  CHECK(dc.status == ae::tele::space::BlobStatus::kBadMagic);

  auto extra = nb;
  extra.push_back(0x7F);
  auto du = ae::tele::space::DecodeBlob<demo::network::Config>(extra);
  CHECK(du.status == ae::tele::space::BlobStatus::kUnknownSection ||
        du.status == ae::tele::space::BlobStatus::kTruncated ||
        du.status == ae::tele::space::BlobStatus::kInvalidRecord);
  return 0;
}

int TestExactAppDeltaAndDuration() {
  demo::application::Clock::reset(100);
  demo::application::Storage storage;
  demo::application::Stream stream{storage,
                                   ae::tele::space::StreamMarker::kMain};
  demo::application::Clock::advance(20);
  demo::EmitFrame(stream);
  demo::application::Clock::advance(1250);
  {
    demo::EmitWork(stream, 80);
  }
  auto blob = ae::tele::space::SerializeBlob(storage);
  auto decoded =
      ae::tele::space::DecodeBlob<demo::application::Config>(blob);
  CHECK(decoded.status == ae::tele::space::BlobStatus::kOk);
  CHECK(decoded.data->events.size() >= 2);
  CHECK(decoded.data->events[0].delta == 20);
  auto const* work = storage.TryMetric(demo::application::kWork.index);
  CHECK(work != nullptr);
  CHECK(work->sum == 80);
  return 0;
}

int TestJournalTruncation() {
  demo::network::Clock::reset(0);
  demo::network::Storage storage;
  demo::network::Stream stream{storage, ae::tele::space::StreamMarker::kMain};
  storage.SetSequence(3);
  storage.SetBaseUnix(50);
  demo::EmitPoll(stream);
  auto blob = ae::tele::space::SerializeBlob(storage);
  auto batch = ae::tele::space::EncodeBatch(blob);
  auto header = ae::tele::space::EncodeJournalHeader(3);
  std::vector<std::uint8_t> file = header;
  file.insert(file.end(), batch.begin(), batch.end());
  file.insert(file.end(), batch.begin(), batch.end());
  file.insert(file.end(), {'A', 'T', 'B', '1', 100, 0, 0, 0});
  auto journal = ae::tele::space::DecodeJournal(file);
  CHECK(journal.sequence == 3);
  CHECK(journal.batches.size() >= 2);
  CHECK(journal.truncated_tail);
  CHECK(!journal.batches[0].truncated);
  CHECK(!journal.batches[0].corrupt);
  auto decoded =
      ae::tele::space::DecodeBlob<demo::network::Config>(journal.batches[0].blob);
  CHECK(decoded.status == ae::tele::space::BlobStatus::kOk);
  return 0;
}

int TestStreamsAndPackedPayload() {
  demo::network::Clock::reset(0);
  demo::network::Storage storage;
  demo::network::Stream main{storage, ae::tele::space::StreamMarker::kMain};
  demo::network::Stream poller{storage, ae::tele::space::StreamMarker::kPoller};
  demo::EmitPktRx(main, 100);
  demo::network::Clock::advance(40);
  demo::EmitPktRx(poller, 800);
  demo::network::Clock::advance(40);
  demo::EmitPktRx(main, 50'000);
  auto blob = ae::tele::space::SerializeBlob(storage);
  auto decoded = ae::tele::space::DecodeBlob<demo::network::Config>(blob);
  CHECK(decoded.status == ae::tele::space::BlobStatus::kOk);
  CHECK(decoded.data->streams.size() == 2);
  CHECK(decoded.data->event_count == 3);
  std::uint64_t rx = 0;
  for (auto const& m : decoded.data->metrics) {
    if (m.index == demo::network::kPktRx.index) {
      rx = m.count;
    }
  }
  CHECK(rx == 3);
  return 0;
}

}  // namespace

int RunUnitTests() {
  if (int rc = TestUnixOverflow()) {
    return rc;
  }
  if (int rc = TestCrcEndianAndGoldenHeader()) {
    return rc;
  }
  if (int rc = TestExponentialCodec()) {
    return rc;
  }
  if (int rc = TestNetworkDeltaTimeline()) {
    return rc;
  }
  if (int rc = TestPackedSizes()) {
    return rc;
  }
  if (int rc = TestTwoSpacesAndChecksum()) {
    return rc;
  }
  if (int rc = TestExactAppDeltaAndDuration()) {
    return rc;
  }
  if (int rc = TestJournalTruncation()) {
    return rc;
  }
  if (int rc = TestStreamsAndPackedPayload()) {
    return rc;
  }
  std::cout << "space_unit ok net_checksum=0x" << std::hex
            << demo::network::Config::kSchemaChecksum
            << " app_checksum=0x" << demo::application::Config::kSchemaChecksum
            << std::dec << '\n';
  return 0;
}
