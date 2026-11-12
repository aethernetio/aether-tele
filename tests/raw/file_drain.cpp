/*
 * Copyright 2026 Aethernet Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "aether-tele/raw.h"
#include "aether-tele/raw/decode.h"
#include "aether-tele/raw/file.h"
#include "aether-tele/raw/wire.h"

#include <unity.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using namespace ae::tele::raw;
namespace fs = std::filesystem;
struct FileIdentity {};
using FileSpace =
    Space<FileIdentity, Options{true, false, false, false, kTime | kPayload}>;
constexpr Tag<FileSpace> kFileTag{7,        0,       0, "file", "checkpoint",
                                  __FILE__, __LINE__};
struct MetricIdentity {};
using MetricSpace = Space<MetricIdentity, Options{false, false, true, true, 0}>;

bool SameMetric(Metric const& value, Metric const& expected) {
  return value.count == expected.count && value.samples == expected.samples &&
         value.sum == expected.sum && value.min == expected.min &&
         value.max == expected.max;
}

struct FailingNativeDrain {
  FileDrain& sink;
  bool fail_next = false;
  bool retry_matches = true;
  std::vector<std::uint8_t> rejected{};
  static bool Callback(void* context, std::span<std::uint8_t const> bytes) {
    auto& self = *static_cast<FailingNativeDrain*>(context);
    if (self.fail_next) {
      self.fail_next = false;
      self.rejected.assign(bytes.begin(), bytes.end());
      return false;
    }
    if (!self.rejected.empty()) {
      self.retry_matches =
          self.retry_matches &&
          std::equal(bytes.begin(), bytes.end(), self.rejected.begin(),
                     self.rejected.end());
      self.rejected.clear();
    }
    return self.sink.Append(bytes);
  }
};

std::vector<std::uint8_t> Frame(std::uint64_t sequence = 0,
                                std::uint64_t ordinal = 0,
                                std::uint64_t time = 1000,
                                std::uint32_t index = 7,
                                std::uint32_t stream = 3) {
  std::vector<std::uint8_t> bytes(80);
  bool const wide = sequence > wire::SequenceCodec::kUpper;
  std::size_t cursor = 5;
  bytes[4] = wire::Control(wire::kLogBlock, kTime | kPayload, wide);
  bool const header =
      wire::Write<wire::StreamCodec>(stream, bytes, cursor) &&
      (wide ? wire::WriteU64(sequence, bytes, cursor)
            : wire::Write<wire::SequenceCodec>(sequence, bytes, cursor)) &&
      wire::WriteU64(time, bytes, cursor);
  bool const row = wire::Write<wire::TagCodec>(index, bytes, cursor) &&
                   wire::Write<wire::TimeCodec>(0, bytes, cursor) &&
                   wire::Write<wire::BlobCodec>(8, bytes, cursor) &&
                   wire::WriteU64(ordinal, bytes, cursor);
  if (!header || !row) throw std::runtime_error("fixture encoding failed");
  auto const block = wire::FinishBlock(bytes, cursor);
  return {block.begin(), block.end()};
}
std::uint64_t DeclaredSize(std::span<std::uint8_t const> bytes) {
  std::size_t cursor = 0;
  std::uint64_t size{};
  if (wire::Read<wire::BlockSizeCodec>(bytes, cursor, size) !=
      wire::ReadStatus::kOk)
    throw std::runtime_error("bad block-size fixture");
  return size;
}
void AppendBytes(std::vector<std::uint8_t>& out,
                 std::span<std::uint8_t const> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}
std::vector<std::uint8_t> ReadFile(fs::path const& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
void WriteFile(fs::path const& path, std::span<std::uint8_t const> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<char const*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file) throw std::runtime_error("fixture write failed");
}
struct TempTree {
  fs::path path;
  TempTree() {
    static std::atomic<unsigned> counter{0};
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / ("tele-file-" + std::to_string(tick) +
                                        "-" + std::to_string(counter++));
    if (!fs::create_directory(path))
      throw std::runtime_error("fixture mkdir failed");
  }
  ~TempTree() {
    std::error_code ignored;
    if (!path.empty() && path.parent_path() == fs::temp_directory_path())
      fs::remove_all(path, ignored);
  }
};

// The same IO interface as the native backend, with deterministic fault points.
// bytes survives Close/Open, modelling durable contents visible on restart.
struct FakeFile {
  static constexpr bool kDirectorySyncSupported = true;
  std::vector<std::uint8_t> bytes;
  bool opened = false;
  std::size_t max_write = std::numeric_limits<std::size_t>::max();
  unsigned writes = 0, syncs = 0, truncates = 0, parents = 0, reads = 0;
  unsigned fail_write = 0, zero_write = 0, throw_write = 0, fail_read = 0;
  std::size_t failed_write_bytes = 0;
  bool fail_truncate = false, throw_after_truncate = false;
  bool zero_read = false, fail_parent = false;
  std::set<unsigned> failed_syncs;
  static bool Error(std::error_code& ec) {
    ec = std::make_error_code(std::errc::io_error);
    return false;
  }
  bool Open(fs::path const&, std::error_code& ec) {
    if (opened) return Error(ec);
    opened = true;
    ec.clear();
    return true;
  }
  bool IsOpen() const { return opened; }
  bool Size(std::uint64_t& size, std::error_code& ec) {
    if (!opened) return Error(ec);
    size = bytes.size();
    ec.clear();
    return true;
  }
  bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> output,
              std::size_t& count, std::error_code& ec) {
    ++reads;
    count = 0;
    ec.clear();
    if (!opened || offset > bytes.size()) return Error(ec);
    if (reads == fail_read) return Error(ec);
    if (zero_read) return true;
    count = std::min(output.size(),
                     bytes.size() - static_cast<std::size_t>(offset));
    std::copy_n(bytes.data() + static_cast<std::size_t>(offset), count,
                output.data());
    return true;
  }
  bool WriteAt(std::uint64_t offset, std::span<std::uint8_t const> input,
               std::size_t& count, std::error_code& ec) {
    ++writes;
    count = 0;
    ec.clear();
    if (!opened || offset > bytes.size()) return Error(ec);
    if (writes == zero_write) return true;
    bool const fail = writes == fail_write || writes == throw_write;
    count = fail ? std::min(failed_write_bytes, input.size())
                 : std::min(max_write, input.size());
    auto end = static_cast<std::size_t>(offset) + count;
    if (end > bytes.size()) bytes.resize(end);
    std::copy_n(input.data(), count,
                bytes.data() + static_cast<std::size_t>(offset));
    if (writes == throw_write)
      throw std::runtime_error("injected write failure");
    return fail ? Error(ec) : true;
  }
  bool Truncate(std::uint64_t size, std::error_code& ec) {
    ++truncates;
    if (!opened || fail_truncate || size > bytes.size()) return Error(ec);
    bytes.resize(static_cast<std::size_t>(size));
    if (throw_after_truncate)
      throw std::runtime_error("injected truncate failure");
    ec.clear();
    return true;
  }
  bool Sync(std::error_code& ec) {
    ++syncs;
    if (!opened || failed_syncs.count(syncs)) return Error(ec);
    ec.clear();
    return true;
  }
  bool SyncParent(std::error_code& ec) {
    ++parents;
    if (fail_parent) return Error(ec);
    ec.clear();
    return true;
  }
  bool Close(std::error_code& ec) {
    opened = false;
    ec.clear();
    return true;
  }
};

void test_packed_block_prefix_recovery_preserves_metric_history() {
  MetricSpace space;
  FakeFile history;
  {
    BasicFileDrain<FakeFile> sink(history);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    Stream<MetricSpace, 28> stream(space, 3, BasicFileDrain<FakeFile>::Callback,
                                   &sink);
    stream.Count(7);
    stream.AddDuration(7, 17);
    TEST_ASSERT_TRUE(stream.DrainMetrics());
  }
  for (auto total : {250u, 251u, 1500u, 1501u}) {
    auto const width = wire::Size<wire::BlockSizeCodec>(total);
    std::vector<std::uint8_t> next(total, 7);
    std::size_t cursor = 0;
    TEST_ASSERT_TRUE(wire::Write<wire::BlockSizeCodec>(total, next, cursor));
    next[cursor++] = 0;
    next[cursor++] = 3;
    next[cursor++] = 0;
    next[cursor++] = 0;
    TEST_ASSERT_TRUE(Decode(next).status == DecodeStatus::kOk);
    for (auto cut :
         {std::size_t{0}, std::size_t{1}, width - 1, width, width + 1,
          next.size() / 2, next.size() - 1, next.size()}) {
      FakeFile file;
      file.bytes = history.bytes;
      AppendBytes(file.bytes, std::span{next}.first(cut));
      BasicFileDrain<FakeFile> sink(file);
      TEST_ASSERT_TRUE(sink.Open("memory"));
      TEST_ASSERT_EQUAL_UINT(cut == next.size() ? 0 : cut,
                             sink.recovery().truncated_bytes);
      Stream<MetricSpace, 28> stream(space, 3,
                                     BasicFileDrain<FakeFile>::Callback, &sink);
      TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
      TEST_ASSERT_TRUE(SameMetric(stream.MetricFor(7), {1, 1, 17, 17, 17}));
      auto const checkpoint = sink.size();
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      TEST_ASSERT_EQUAL_UINT64(checkpoint, sink.size());
      stream.Count(7);
      stream.AddDuration(7, 5);
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      auto decoded = Decode(file.bytes);
      TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
      TEST_ASSERT_TRUE(
          SameMetric(RestoreMetric(decoded.records, 7), {2, 2, 22, 5, 17}));
    }
    // A complete size prefix plus an invalid control byte is corrupt even
    // when the rest of the body is missing. Never repair it by truncation.
    FakeFile corrupt;
    corrupt.bytes = history.bytes;
    next[width] = 0x40;
    AppendBytes(corrupt.bytes, std::span{next}.first(width + 1));
    auto const original = corrupt.bytes;
    BasicFileDrain<FakeFile> reject(corrupt);
    TEST_ASSERT_FALSE(reject.Open("memory"));
    TEST_ASSERT_TRUE(reject.status().error == FileError::kMalformedFile);
    TEST_ASSERT_TRUE(corrupt.bytes == original);
    TEST_ASSERT_EQUAL_UINT(0, corrupt.truncates);
  }
  for (std::uint8_t invalid = 0; invalid < kMinBlockSize; ++invalid) {
    FakeFile file;
    file.bytes = history.bytes;
    file.bytes.push_back(invalid);
    auto original = file.bytes;
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_FALSE(sink.Open("memory"));
    TEST_ASSERT_TRUE(sink.status().error == FileError::kMalformedFile);
    TEST_ASSERT_TRUE(file.bytes == original);
    TEST_ASSERT_EQUAL_UINT(0, file.truncates);
  }
  FakeFile oversized;
  oversized.bytes = {0xff, 0xe2, 0x00,
                     0x00};  // 1501, complete prefix but missing body
  BasicFileDrain<FakeFile> limited(oversized);
  TEST_ASSERT_FALSE(limited.Open("memory", FileOptions{1024, true}));
  TEST_ASSERT_TRUE(limited.status().error == FileError::kRecordTooLarge);
  TEST_ASSERT_EQUAL_UINT(0, oversized.truncates);
}

void test_native_append_and_reopen_preserve_bytes() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  auto first = Frame(0, 10), second = Frame(1, 20);
  std::vector<std::uint8_t> expected = first;
  AppendBytes(expected, second);
  FileDrain sink;
  TEST_ASSERT_TRUE(sink.Open(path));
  TEST_ASSERT_TRUE(sink.Append(first));
  TEST_ASSERT_TRUE(FileDrain::Callback(&sink, second));
  TEST_ASSERT_EQUAL_UINT32(expected.size(), sink.size());
  TEST_ASSERT_TRUE(sink.Close());
  TEST_ASSERT_TRUE(ReadFile(path) == expected);
  TEST_ASSERT_TRUE(sink.Open(path));
  TEST_ASSERT_EQUAL_UINT32(expected.size(), sink.recovery().valid_size);
  TEST_ASSERT_EQUAL_UINT32(0, sink.recovery().truncated_bytes);
  auto restarted = Frame(0, 30, 2000);
  TEST_ASSERT_TRUE(sink.Append(restarted));
  TEST_ASSERT_TRUE(sink.Close());
  AppendBytes(expected, restarted);
  TEST_ASSERT_TRUE(ReadFile(path) == expected);
  auto decoded = Decode(expected);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT32(3, decoded.records.size());
  TEST_ASSERT_FALSE(FileDrain::Callback(nullptr, first));
}

void test_native_rejects_invalid_batch_before_writing_prefix() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  FileDrain sink;
  TEST_ASSERT_TRUE(sink.Open(path));
  auto first = Frame();
  TEST_ASSERT_TRUE(sink.Append(first));
  auto bad = Frame(1);
  auto cut = Frame(2);
  cut.pop_back();
  AppendBytes(bad, cut);
  TEST_ASSERT_FALSE(sink.Append(bad));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kInvalidInput);
  TEST_ASSERT_EQUAL_UINT32(first.size(), sink.size());
  TEST_ASSERT_TRUE(sink.state() == FileState::kReady);
  TEST_ASSERT_TRUE(sink.Close());
  TEST_ASSERT_TRUE(ReadFile(path) == first);
}

void test_native_repairs_only_an_incomplete_final_record() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  auto first = Frame(0, 10), next = Frame(1, 20);
  for (std::size_t cut = 0; cut <= next.size(); ++cut) {
    auto original = first;
    AppendBytes(original, std::span(next).first(cut));
    WriteFile(path, original);
    FileDrain sink;
    TEST_ASSERT_TRUE(sink.Open(path));
    std::size_t const valid =
        cut == next.size() ? original.size() : first.size();
    TEST_ASSERT_EQUAL_UINT32(original.size(), sink.recovery().original_size);
    TEST_ASSERT_EQUAL_UINT32(valid, sink.recovery().valid_size);
    TEST_ASSERT_EQUAL_UINT32(original.size() - valid,
                             sink.recovery().truncated_bytes);
    TEST_ASSERT_TRUE(sink.Close());
    original.resize(valid);
    TEST_ASSERT_TRUE(ReadFile(path) == original);
  }
}

void test_native_malformed_or_refused_tail_is_not_modified() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  auto original = Frame();
  auto malformed = Frame(1);
  malformed[1] |= 0x40;  // Reserved control bit.
  AppendBytes(original, malformed);
  WriteFile(path, original);
  FileDrain invalid;
  TEST_ASSERT_FALSE(invalid.Open(path));
  TEST_ASSERT_TRUE(invalid.status().error == FileError::kMalformedFile);
  TEST_ASSERT_TRUE(ReadFile(path) == original);
  auto partial = Frame();
  partial.push_back(44);
  WriteFile(path, partial);
  FileOptions refuse;
  refuse.repair_incomplete_tail = false;
  FileDrain no_repair;
  TEST_ASSERT_FALSE(no_repair.Open(path, refuse));
  TEST_ASSERT_TRUE(no_repair.status().error == FileError::kIncompleteFile);
  TEST_ASSERT_TRUE(ReadFile(path) == partial);
  WriteFile(path, Frame());
  FileOptions small;
  small.max_record_bytes = static_cast<std::uint32_t>(Frame().size() - 1);
  FileDrain limited;
  TEST_ASSERT_FALSE(limited.Open(path, small));
  TEST_ASSERT_TRUE(limited.status().error == FileError::kRecordTooLarge);
  TEST_ASSERT_TRUE(ReadFile(path) == Frame());
}

void test_native_write_owner_is_exclusive() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  FileDrain first, second;
  TEST_ASSERT_TRUE(first.Open(path));
  TEST_ASSERT_FALSE(second.Open(path));
  TEST_ASSERT_TRUE(first.Append(Frame()));
  TEST_ASSERT_TRUE(first.Close());
  TEST_ASSERT_TRUE(second.Open(path));
  TEST_ASSERT_TRUE(second.Close());
}

void test_native_shared_sink_serializes_four_writer_batches() {
  TempTree tree;
  auto path = tree.path / "capture.raw";
  FileDrain sink;
  FileSpace space;
  TEST_ASSERT_TRUE(sink.Open(path));
  std::atomic<bool> failed{false};
  std::array<std::thread, 4> writers;
  for (std::uint32_t id = 0; id != writers.size(); ++id) {
    writers[id] = std::thread([&, id] {
      Stream<FileSpace, 96> stream(space, id, FileDrain::Callback, &sink);
      for (std::uint64_t n = 0; n != 60; ++n) {
        std::array<std::uint8_t, 8> payload{};
        Store64(payload.data(), n);
        if (!stream.Emit(kFileTag, Kind::kLog, space.Runtime(), Data{payload},
                         0, 1000 + n))
          failed = true;
      }
      if (!stream.Drain()) failed = true;
    });
  }
  for (auto& writer : writers) writer.join();
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.Close());
  auto bytes = ReadFile(path);
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT32(240, decoded.records.size());
  std::array<unsigned, 4> count{};
  for (auto const& record : decoded.records) {
    TEST_ASSERT_TRUE(record.stream < count.size());
    TEST_ASSERT_EQUAL_UINT32(count[record.stream], record.sequence);
    TEST_ASSERT_EQUAL_UINT32(8, record.payload.size());
    TEST_ASSERT_EQUAL_UINT64(count[record.stream],
                             Load64(record.payload.data()));
    ++count[record.stream];
  }
  for (auto n : count) TEST_ASSERT_EQUAL_UINT32(60, n);
}

void test_short_writes_are_completed_before_acknowledgement() {
  FakeFile file;
  file.max_write = 3;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  auto frame = Frame();
  auto before = file.syncs;
  TEST_ASSERT_TRUE(sink.Append(frame));
  TEST_ASSERT_TRUE(file.bytes == frame);
  TEST_ASSERT_TRUE(file.writes > 1);
  TEST_ASSERT_EQUAL_UINT32(before + 1, file.syncs);
}

void test_partial_write_error_rolls_back_and_retry_is_exact() {
  FakeFile file;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  auto first = Frame(0, 10), next = Frame(1, 20);
  TEST_ASSERT_TRUE(sink.Append(first));
  file.max_write = 5;
  file.fail_write = file.writes + 2;
  file.failed_write_bytes = 2;
  TEST_ASSERT_FALSE(sink.Append(next));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kWrite);
  TEST_ASSERT_TRUE(sink.state() == FileState::kReady);
  TEST_ASSERT_TRUE(file.bytes == first);
  TEST_ASSERT_EQUAL_UINT32(first.size(), sink.size());
  file.fail_write = 0;
  TEST_ASSERT_TRUE(sink.Append(next));
  AppendBytes(first, next);
  TEST_ASSERT_TRUE(file.bytes == first);
}

void test_zero_write_and_throwing_write_do_not_acknowledge() {
  for (bool throwing : {false, true}) {
    FakeFile file;
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    auto first = Frame();
    TEST_ASSERT_TRUE(sink.Append(first));
    if (throwing) {
      file.throw_write = file.writes + 1;
      file.failed_write_bytes = 5;
    } else
      file.zero_write = file.writes + 1;
    TEST_ASSERT_FALSE(sink.Append(Frame(1)));
    TEST_ASSERT_TRUE(file.bytes == first);
    TEST_ASSERT_TRUE(sink.state() == FileState::kReady);
  }
}

void test_sync_failure_rolls_back_before_successful_retry() {
  FakeFile file;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  auto first = Frame();
  TEST_ASSERT_TRUE(sink.Append(first));
  auto next = Frame(1);
  file.failed_syncs.insert(file.syncs + 1);
  TEST_ASSERT_FALSE(sink.Append(next));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kSync);
  TEST_ASSERT_TRUE(file.bytes == first);
  TEST_ASSERT_TRUE(sink.state() == FileState::kReady);
  TEST_ASSERT_TRUE(sink.Append(next));
  AppendBytes(first, next);
  TEST_ASSERT_TRUE(file.bytes == first);
}

void test_failed_file_callback_retains_exact_stream_bytes() {
  FakeFile file;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  FileSpace space;
  Stream<FileSpace, 96> stream(space, 3, BasicFileDrain<FakeFile>::Callback,
                               &sink);
  TEST_ASSERT_TRUE(
      stream.Emit(kFileTag, Kind::kLog, space.Runtime(), {}, 0, 1000));
  std::vector<std::uint8_t> pending(stream.pending().begin(),
                                    stream.pending().end());
  file.fail_write = file.writes + 1;
  file.failed_write_bytes = 11;
  TEST_ASSERT_FALSE(stream.Drain());
  TEST_ASSERT_TRUE(file.bytes.empty());
  TEST_ASSERT_EQUAL_UINT32(pending.size(), stream.pending().size());
  TEST_ASSERT_EQUAL_MEMORY(pending.data(), stream.pending().data(),
                           pending.size());
  file.fail_write = 0;
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_TRUE(stream.pending().empty());
  TEST_ASSERT_TRUE(file.bytes == pending);
  TEST_ASSERT_TRUE(stream.Drain());
  TEST_ASSERT_TRUE(file.bytes == pending);
}

void test_failed_repair_stays_poisoned_until_explicit_close() {
  for (unsigned fault = 0; fault != 3; ++fault) {
    FakeFile file;
    file.bytes = Frame();
    file.bytes.push_back(44);
    if (fault == 0)
      file.failed_syncs.insert(1);
    else if (fault == 1)
      file.throw_after_truncate = true;
    else
      file.fail_parent = true;
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_FALSE(sink.Open("memory"));
    TEST_ASSERT_TRUE(sink.state() == FileState::kPoisoned);
    TEST_ASSERT_TRUE(file.opened);
    TEST_ASSERT_TRUE(file.bytes == Frame());
    auto writes = file.writes;
    TEST_ASSERT_FALSE(sink.Append(Frame(1)));
    TEST_ASSERT_EQUAL_UINT32(writes, file.writes);
    (void)sink.Close();
    TEST_ASSERT_FALSE(file.opened);
    TEST_ASSERT_FALSE(sink.Open("memory"));
  }
}

void test_parent_sync_failure_without_repair_preserves_original_file() {
  FakeFile file;
  file.bytes = Frame();
  file.fail_parent = true;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_FALSE(sink.Open("memory"));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kDirectorySync);
  TEST_ASSERT_TRUE(sink.state() == FileState::kClosed);
  TEST_ASSERT_FALSE(file.opened);
  TEST_ASSERT_TRUE(file.bytes == Frame());
  TEST_ASSERT_EQUAL_UINT32(0, file.truncates);
}

void test_visible_malformed_partial_header_is_not_repaired() {
  for (std::size_t cut = 5; cut < Frame().size(); ++cut) {
    FakeFile file;
    file.bytes = Frame();
    auto malformed = Frame(1);
    malformed[1] |= 0x40;  // Corruption is already visible in the prefix.
    AppendBytes(file.bytes, std::span(malformed).first(cut));
    auto original = file.bytes;
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_FALSE(sink.Open("memory"));
    TEST_ASSERT_TRUE(sink.status().error == FileError::kMalformedFile);
    TEST_ASSERT_TRUE(file.bytes == original);
    TEST_ASSERT_EQUAL_UINT32(0, file.truncates);
  }
}

void test_unconfirmed_rollback_poison_is_sticky() {
  for (bool truncate_failure : {false, true}) {
    FakeFile file;
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    TEST_ASSERT_TRUE(sink.Append(Frame()));
    if (truncate_failure) {
      file.fail_write = file.writes + 1;
      file.failed_write_bytes = 7;
      file.fail_truncate = true;
    } else {
      file.failed_syncs.insert(file.syncs + 1);
      file.failed_syncs.insert(file.syncs + 2);
    }
    TEST_ASSERT_FALSE(sink.Append(Frame(1)));
    TEST_ASSERT_TRUE(sink.status().error == FileError::kRollbackFailed);
    TEST_ASSERT_TRUE(sink.state() == FileState::kPoisoned);
    auto writes = file.writes;
    TEST_ASSERT_FALSE(sink.Append(Frame(1)));
    TEST_ASSERT_EQUAL_UINT32(writes, file.writes);
    (void)sink.Close();
    TEST_ASSERT_FALSE(sink.Open("memory"));
    TEST_ASSERT_TRUE(sink.state() == FileState::kPoisoned);
  }
}

void test_open_read_failure_and_record_limit_never_repair_data() {
  FakeFile file;
  file.bytes = Frame();
  auto original = file.bytes;
  file.zero_read = true;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_FALSE(sink.Open("memory"));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kRead);
  TEST_ASSERT_TRUE(file.bytes == original);
  TEST_ASSERT_EQUAL_UINT32(0, file.truncates);
}

void test_external_size_change_never_gets_overwritten() {
  FakeFile file;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  TEST_ASSERT_TRUE(sink.Append(Frame()));
  file.bytes.push_back(0xaa);
  auto observed = file.bytes;
  TEST_ASSERT_FALSE(sink.Append(Frame(1)));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kExternalChange);
  TEST_ASSERT_TRUE(sink.state() == FileState::kPoisoned);
  TEST_ASSERT_TRUE(file.bytes == observed);
}

void test_metric_restore_survives_write_and_sync_failures_without_recounting() {
  for (bool sync_failure : {false, true}) {
    FakeFile file;
    MetricSpace space;
    for (unsigned boot = 0; boot != 4; ++boot) {
      BasicFileDrain<FakeFile> sink(file);
      TEST_ASSERT_TRUE(sink.Open("memory"));
      Stream<MetricSpace, 28> stream(space, 3,
                                     BasicFileDrain<FakeFile>::Callback, &sink);
      TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
      TEST_ASSERT_TRUE(SameMetric(
          stream.MetricFor(7),
          boot ? Metric{2u * boot, 2u * boot, 12u * boot, 5, 7} : Metric{}));
      auto saved = file.bytes;
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      TEST_ASSERT_TRUE(file.bytes == saved);
      stream.Count(7);
      stream.AddDuration(7, 5);
      if (sync_failure)
        file.failed_syncs.insert(file.syncs + 1);
      else {
        file.fail_write = file.writes + 1;
        file.failed_write_bytes = 11;
      }
      TEST_ASSERT_FALSE(stream.DrainMetrics());
      TEST_ASSERT_TRUE(file.bytes == saved);
      stream.Count(7);
      stream.AddDuration(7, 7);
      file.fail_write = 0;
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      auto complete = file.bytes;
      TEST_ASSERT_TRUE(stream.DrainMetrics());
      TEST_ASSERT_TRUE(file.bytes == complete);
      auto decoded = Decode(file.bytes);
      TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
      TEST_ASSERT_TRUE(SameMetric(
          RestoreMetric(decoded.records, 7),
          Metric{2u * (boot + 1), 2u * (boot + 1), 12u * (boot + 1), 5, 7}));
      TEST_ASSERT_TRUE(
          SameMetric(stream.MetricFor(7), RestoreMetric(decoded.records, 7)));
      TEST_ASSERT_TRUE(sink.Close());
    }
  }
}

void test_restart_restores_only_accepted_tags_after_partial_metric_drain() {
  FakeFile file;
  MetricSpace space;
  {
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    Stream<MetricSpace, 28> stream(space, 3, BasicFileDrain<FakeFile>::Callback,
                                   &sink);
    TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
    stream.Count(7);
    stream.AddDuration(7, 5);
    stream.Count(8);
    stream.AddDuration(8, 9);
    file.fail_write = file.writes + 2;
    file.failed_write_bytes = 13;
    TEST_ASSERT_FALSE(stream.DrainMetrics());
    auto decoded = Decode(file.bytes);
    TEST_ASSERT_EQUAL_UINT32(1, decoded.records.size());
    TEST_ASSERT_EQUAL_UINT32(7, decoded.records[0].index);
    // Simulate process death: the failed second tag is only in volatile RAM.
  }
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  Stream<MetricSpace, 28> stream(space, 3, BasicFileDrain<FakeFile>::Callback,
                                 &sink);
  TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
  TEST_ASSERT_TRUE(SameMetric(stream.MetricFor(7), Metric{1, 1, 5, 5, 5}));
  TEST_ASSERT_TRUE(SameMetric(stream.MetricFor(8), Metric{}));
  stream.Count(7);
  stream.AddDuration(7, 7);
  stream.Count(8);
  stream.AddDuration(8, 11);
  TEST_ASSERT_TRUE(stream.DrainMetrics());
  auto decoded = Decode(file.bytes);
  TEST_ASSERT_TRUE(
      SameMetric(RestoreMetric(decoded.records, 7), Metric{2, 2, 12, 5, 7}));
  TEST_ASSERT_TRUE(
      SameMetric(RestoreMetric(decoded.records, 8), Metric{1, 1, 11, 11, 11}));
}

void test_file_restore_capacity_and_read_failures_leave_stream_unmodified() {
  FakeFile file;
  MetricSpace space;
  BasicFileDrain<FakeFile> sink(file);
  TEST_ASSERT_TRUE(sink.Open("memory"));
  {
    Stream<MetricSpace, 28> original(space, 3,
                                     BasicFileDrain<FakeFile>::Callback, &sink);
    original.Count(7);
    original.Count(8);
    TEST_ASSERT_TRUE(original.DrainMetrics());
  }
  Stream<MetricSpace, 28, 1> too_small(space, 3);
  TEST_ASSERT_FALSE(sink.RestoreMetrics(too_small));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kMetricCapacity);
  TEST_ASSERT_TRUE(SameMetric(too_small.MetricFor(7), Metric{}));
  TEST_ASSERT_TRUE(SameMetric(too_small.MetricFor(8), Metric{}));
  std::array<MetricTotal, 1> seed{{{9, Metric{4, 0, 0, 0, 0}}}};
  TEST_ASSERT_TRUE(too_small.RestoreMetrics(seed));

  Stream<MetricSpace, 28> fresh(space, 3);
  file.zero_read = true;
  TEST_ASSERT_FALSE(sink.RestoreMetrics(fresh));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kRead);
  TEST_ASSERT_TRUE(SameMetric(fresh.MetricFor(7), Metric{}));
  file.zero_read = false;
  // Fail after aggregating the first tag. No history may leak into RAM until
  // the complete scan succeeds, and retry must start from zero again.
  file.fail_read = file.reads + 3;
  FileStatus outcome;
  auto const checkpoint = file.bytes;
  TEST_ASSERT_FALSE(sink.RestoreMetrics(fresh, &outcome));
  TEST_ASSERT_TRUE(outcome.error == FileError::kRead);
  TEST_ASSERT_EQUAL_UINT64(DeclaredSize(file.bytes), outcome.offset);
  TEST_ASSERT_TRUE(file.bytes == checkpoint);
  TEST_ASSERT_TRUE(SameMetric(fresh.MetricFor(7), Metric{}));
  TEST_ASSERT_TRUE(SameMetric(fresh.MetricFor(8), Metric{}));
  file.fail_read = 0;
  TEST_ASSERT_TRUE(sink.RestoreMetrics(fresh));
  TEST_ASSERT_EQUAL_UINT64(1, fresh.MetricFor(7).count);
  TEST_ASSERT_EQUAL_UINT64(1, fresh.MetricFor(8).count);
  TEST_ASSERT_FALSE(sink.RestoreMetrics(fresh));
  TEST_ASSERT_TRUE(sink.status().error == FileError::kRestoreState);
}

void test_restore_error_outcome_survives_another_writer() {
  FakeFile file;
  BasicFileDrain<FakeFile> sink(file);
  MetricSpace space;
  TEST_ASSERT_TRUE(sink.Open("memory"));
  Stream<MetricSpace, 28> writer(space, 9, BasicFileDrain<FakeFile>::Callback,
                                 &sink);
  writer.Count(7);
  TEST_ASSERT_TRUE(writer.DrainMetrics());
  Stream<MetricSpace, 28> restored(space, 3);
  FileStatus outcome;
  file.zero_read = true;
  TEST_ASSERT_FALSE(sink.RestoreMetrics(restored, &outcome));
  file.zero_read = false;
  std::thread other([&] {
    writer.Count(7);
    writer.DrainMetrics();
  });
  other.join();
  TEST_ASSERT_TRUE(sink.status().error == FileError::kNone);
  TEST_ASSERT_TRUE(outcome.error == FileError::kRead);
  TEST_ASSERT_TRUE(outcome.io_error == std::errc::io_error);
  TEST_ASSERT_TRUE(SameMetric(restored.MetricFor(7), Metric{}));
  TEST_ASSERT_TRUE(sink.RestoreMetrics(restored, &outcome));
  TEST_ASSERT_TRUE(outcome.error == FileError::kNone);
  TEST_ASSERT_FALSE(sink.RestoreMetrics(restored, &outcome));
  TEST_ASSERT_TRUE(outcome.error == FileError::kRestoreState);
  TEST_ASSERT_TRUE(sink.Close());
  Stream<MetricSpace, 28> closed(space, 3);
  TEST_ASSERT_FALSE(sink.RestoreMetrics(closed, &outcome));
  TEST_ASSERT_TRUE(outcome.error == FileError::kNotOpen);
}

void test_metric_restart_at_every_frame_byte_preserves_complete_totals() {
  // The last frame may be complete even though its callback never returned.
  // Keep that frame exactly once; partial frames contribute nothing.
  FakeFile fixture;
  MetricSpace space;
  {
    BasicFileDrain<FakeFile> sink(fixture);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    Stream<MetricSpace, 28> writer(space, 3, BasicFileDrain<FakeFile>::Callback,
                                   &sink);
    writer.Count(7);
    writer.AddDuration(7, 10);
    TEST_ASSERT_TRUE(writer.DrainMetrics());
    writer.Count(7);
    writer.AddDuration(7, 5);
    TEST_ASSERT_TRUE(writer.DrainMetrics());
  }
  auto const first_size = DeclaredSize(fixture.bytes);
  auto const second_size =
      DeclaredSize(std::span{fixture.bytes}.subspan(first_size));
  TEST_ASSERT_EQUAL_UINT32(first_size + second_size, fixture.bytes.size());
  for (std::size_t cut = 0; cut <= second_size; ++cut) {
    FakeFile file;
    file.bytes.assign(fixture.bytes.begin(),
                      fixture.bytes.begin() + first_size + cut);
    BasicFileDrain<FakeFile> sink(file);
    TEST_ASSERT_TRUE(sink.Open("memory"));
    TEST_ASSERT_EQUAL_UINT64(cut == second_size ? 0 : cut,
                             sink.recovery().truncated_bytes);
    Stream<MetricSpace, 28> stream(space, 3, BasicFileDrain<FakeFile>::Callback,
                                   &sink);
    TEST_ASSERT_TRUE(sink.RestoreMetrics(stream));
    bool const complete = cut == second_size;
    TEST_ASSERT_TRUE(
        SameMetric(stream.MetricFor(7), complete ? Metric{2, 2, 15, 5, 10}
                                                 : Metric{1, 1, 10, 10, 10}));
    auto const checkpoint = file.bytes;
    TEST_ASSERT_TRUE(stream.DrainMetrics());
    TEST_ASSERT_TRUE(file.bytes == checkpoint);
    stream.Count(7);
    stream.AddDuration(7, 7);
    TEST_ASSERT_TRUE(stream.DrainMetrics());
    TEST_ASSERT_TRUE(SameMetric(
        RestoreMetric(Decode(file.bytes).records, 7),
        complete ? Metric{3, 3, 22, 5, 10} : Metric{2, 2, 17, 7, 10}));
  }
}

void test_concurrent_restore_retry_and_drain_keep_each_writer_totals() {
  TempTree tree;
  auto path = tree.path / "concurrent-metrics.raw";
  FileDrain sink;
  MetricSpace space;
  TEST_ASSERT_TRUE(sink.Open(path));
  constexpr unsigned kWriters = 4, kUpdates = 12;
  for (unsigned id = 0; id != kWriters; ++id) {
    Stream<MetricSpace, 28> stream(space, id, FileDrain::Callback, &sink);
    stream.Count(7);
    stream.AddDuration(7, 10 + id);
    TEST_ASSERT_TRUE(stream.DrainMetrics());
  }
  std::atomic<bool> first_started{false}, done{false}, failed{false};
  std::array<Metric, kWriters> live{};
  std::array<std::thread, kWriters> writers;
  std::thread observer([&] {
    while (!done.load()) {
      if (sink.state() != FileState::kReady || sink.size() == 0) failed = true;
      (void)sink.status();
      (void)sink.recovery();
      std::this_thread::yield();
    }
  });
  for (unsigned id = 0; id != kWriters; ++id) {
    writers[id] = std::thread([&, id] {
      if (id != 0)
        while (!first_started.load()) std::this_thread::yield();
      FailingNativeDrain callback{sink};
      Stream<MetricSpace, 28> stream(space, id, FailingNativeDrain::Callback,
                                     &callback);
      FileStatus outcome;
      if (!sink.RestoreMetrics(stream, &outcome) ||
          outcome.error != FileError::kNone ||
          !SameMetric(stream.MetricFor(7),
                      Metric{1, 1, 10u + id, 10u + id, 10u + id}))
        failed = true;
      for (unsigned i = 0; i != kUpdates; ++i) {
        stream.Count(7);
        stream.AddDuration(7, i + 1);
        callback.fail_next = true;
        if (stream.DrainMetrics()) failed = true;
        first_started = true;
        stream.Count(7);
        stream.AddDuration(7, i + 2);
        if (!stream.DrainMetrics() || !callback.retry_matches) failed = true;
      }
      live[id] = stream.MetricFor(7);
    });
  }
  for (auto& writer : writers) writer.join();
  done = true;
  observer.join();
  TEST_ASSERT_FALSE(failed.load());
  TEST_ASSERT_TRUE(sink.Close());
  auto bytes = ReadFile(path);
  auto decoded = Decode(bytes);
  TEST_ASSERT_TRUE(decoded.status == DecodeStatus::kOk);
  TEST_ASSERT_EQUAL_UINT32(kWriters * (1 + 2 * kUpdates),
                           decoded.records.size());
  TEST_ASSERT_TRUE(sink.Open(path));
  for (unsigned id = 0; id != kWriters; ++id) {
    auto const expected = Metric{1 + 2 * kUpdates, 1 + 2 * kUpdates,
                                 10u + id + kUpdates * (kUpdates + 2), 1,
                                 std::max(10u + id, kUpdates + 1)};
    TEST_ASSERT_TRUE(SameMetric(live[id], expected));
    Stream<MetricSpace, 28> restored(space, id, FileDrain::Callback, &sink);
    TEST_ASSERT_TRUE(sink.RestoreMetrics(restored));
    TEST_ASSERT_TRUE(SameMetric(restored.MetricFor(7), expected));
    auto const checkpoint = sink.size();
    TEST_ASSERT_TRUE(restored.DrainMetrics());
    TEST_ASSERT_EQUAL_UINT64(checkpoint, sink.size());
  }
}

Metric ExpectedBootMetric(unsigned boots, bool second) {
  if (boots == 0) return {};
  if (second)
    return {boots, boots, 100u * boots + boots * (boots - 1u) / 2u, 100,
            99u + boots};
  return {2u * boots, 2u * boots, boots * (boots + 2u), 1, boots + 1u};
}

// Stop inside the storage callback, before Stream observes its acknowledgement.
// A native sync pins the chosen prefix for deterministic process-crash tests;
// this does not simulate arbitrary hardware reordering during a power failure.
struct CrashNativeFile {
  static constexpr bool kDirectorySyncSupported =
      file_internal::NativeFile::kDirectorySyncSupported;
  file_internal::NativeFile native;
  std::string point;
  bool armed = false;
  bool Open(fs::path const& path, std::error_code& ec) {
    return native.Open(path, ec);
  }
  bool Size(std::uint64_t& size, std::error_code& ec) {
    return native.Size(size, ec);
  }
  bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> out,
              std::size_t& count, std::error_code& ec) {
    return native.ReadAt(offset, out, count, ec);
  }
  bool WriteAt(std::uint64_t offset, std::span<std::uint8_t const> bytes,
               std::size_t& count, std::error_code& ec) {
    bool const partial = armed && (point == "partial" || point == "rollback");
    auto const input =
        partial ? bytes.first(std::max<std::size_t>(1, bytes.size() / 2))
                : bytes;
    if (!native.WriteAt(offset, input, count, ec)) return false;
    if (armed && point == "rollback") {
      ec = std::make_error_code(std::errc::io_error);
      return false;
    }
    if (armed &&
        (point == "partial" || (point == "full" && count == bytes.size())))
      std::_Exit(native.Sync(ec) ? 0 : 90);
    return true;
  }
  bool Sync(std::error_code& ec) {
    if (!native.Sync(ec)) return false;
    if (armed && point == "synced") std::_Exit(0);
    return true;
  }
  bool Truncate(std::uint64_t size, std::error_code& ec) {
    if (!native.Truncate(size, ec)) return false;
    if (armed && point == "rollback") std::_Exit(native.Sync(ec) ? 0 : 91);
    return true;
  }
  bool SyncParent(std::error_code& ec) { return native.SyncParent(ec); }
  bool Close(std::error_code& ec) { return native.Close(ec); }
  bool IsOpen() const { return native.IsOpen(); }
};

int CrashMetricProcess(fs::path const& path, std::string const& point) {
  CrashNativeFile file;
  file.point = point;
  BasicFileDrain<CrashNativeFile> sink(file);
  if (!sink.Open(path)) return 80;
  MetricSpace space;
  Stream<MetricSpace, 28> stream(
      space, 3, BasicFileDrain<CrashNativeFile>::Callback, &sink);
  if (!sink.RestoreMetrics(stream)) return 81;
  stream.Count(7);
  stream.AddDuration(7, 10);
  if (!stream.DrainMetrics()) return 82;
  file.armed = true;
  stream.Count(7);
  stream.AddDuration(7, 5);
  (void)stream.DrainMetrics();
  return 83;  // Every supported fault point must exit inside the callback.
}

int RecoverMetricProcess(fs::path const& path, std::string const& point,
                         bool add_update) {
  bool const full = point == "full" || point == "synced";
  Metric before = full ? Metric{2, 2, 15, 5, 10} : Metric{1, 1, 10, 10, 10};
  Metric after = full ? Metric{3, 3, 22, 5, 10} : Metric{2, 2, 17, 7, 10};
  FileDrain sink;
  if (!sink.Open(path)) return 84;
  MetricSpace space;
  Stream<MetricSpace, 28> stream(space, 3, FileDrain::Callback, &sink);
  if (!sink.RestoreMetrics(stream) ||
      !SameMetric(stream.MetricFor(7), add_update ? before : after))
    return 85;
  auto const checkpoint = sink.size();
  if (!stream.DrainMetrics() || sink.size() != checkpoint) return 86;
  if (add_update) {
    stream.Count(7);
    stream.AddDuration(7, 7);
    if (!stream.DrainMetrics() || !SameMetric(stream.MetricFor(7), after))
      return 87;
    if (sink.size() <= checkpoint) return 88;
  }
  return sink.Close() ? 0 : 89;
}

int MetricProcessMode(std::string const& mode, fs::path const& path,
                      unsigned boot) {
  FileDrain sink;
  if (!sink.Open(path)) return 30;
  MetricSpace space;
  FailingNativeDrain callback{sink};
  Stream<MetricSpace, 28> first(space, 3, FailingNativeDrain::Callback,
                                &callback);
  Stream<MetricSpace, 28> second(space, 9, FileDrain::Callback, &sink);
  if (!sink.RestoreMetrics(first) || !sink.RestoreMetrics(second)) return 31;
  if (!SameMetric(first.MetricFor(7), ExpectedBootMetric(boot, false)) ||
      !SameMetric(second.MetricFor(7), ExpectedBootMetric(boot, true)))
    return 32;
  auto checkpoint = sink.size();
  if (!first.DrainMetrics() || !second.DrainMetrics() ||
      sink.size() != checkpoint)
    return 33;
  if (mode == "--metrics-check") return sink.Close() ? 0 : 34;
  first.Count(7);
  first.AddDuration(7, boot + 1u);
  callback.fail_next = true;
  if (first.DrainMetrics() || sink.size() != checkpoint) return 35;
  first.Count(7);
  first.AddDuration(7, boot + 2u);
  if (!first.DrainMetrics() || !callback.retry_matches ||
      !callback.rejected.empty())
    return 36;
  second.Count(7);
  second.AddDuration(7, 100u + boot);
  if (!second.DrainMetrics()) return 37;
  if (!SameMetric(first.MetricFor(7), ExpectedBootMetric(boot + 1u, false)) ||
      !SameMetric(second.MetricFor(7), ExpectedBootMetric(boot + 1u, true)))
    return 38;
  checkpoint = sink.size();
  if (!first.DrainMetrics() || !second.DrainMetrics() ||
      sink.size() != checkpoint)
    return 39;
  // All boots end abruptly: no stop, explicit Close, or destructors.
  std::_Exit(0);
}

int ProcessMode(std::string const& mode, fs::path const& path,
                fs::path const& ready = {}) {
  if (mode == "--metrics-crash")
    return CrashMetricProcess(path, ready.string());
  if (mode == "--metrics-recover" || mode == "--metrics-recovered-check")
    return RecoverMetricProcess(path, ready.string(),
                                mode == "--metrics-recover");
  if (mode == "--metrics-boot" || mode == "--metrics-check")
    return MetricProcessMode(mode, path,
                             static_cast<unsigned>(std::stoul(ready.string())));
  if (mode == "--verify") {
    auto bytes = ReadFile(path);
    auto decoded = Decode(bytes);
    if (decoded.status != DecodeStatus::kOk || decoded.records.size() != 13)
      return 2;
    unsigned starts = 0, stops = 0, value = 0;
    for (auto const& record : decoded.records) {
      if (record.index == 100) {
        ++starts;
        if (record.sequence != 0) return 3;
      } else if (record.index == 101)
        ++stops;
      else if (record.index == 7) {
        if (record.payload.size() != 8 ||
            Load64(record.payload.data()) != value++)
          return 4;
      } else
        return 5;
    }
    return starts == 2 && stops == 1 && value == 10 ? 0 : 6;
  }
  FileDrain sink;
  if (mode == "--exclusive-probe") return sink.Open(path) ? 7 : 0;
  if (!sink.Open(path)) return 8;
  if (mode == "--hold") {
    std::ofstream signal(ready);
    signal << "ready";
    signal.close();
    std::string release;
    std::getline(std::cin, release);
    return sink.Close() ? 0 : 9;
  }
  bool const resumed = mode == "--resume";
  if (!resumed && mode != "--write") return 10;
  if (resumed && sink.recovery().truncated_bytes != 17) return 11;
  auto start = resumed ? 2000u : 1000u;
  if (!sink.Append(Frame(0, 0, start, 100))) return 12;
  for (unsigned i = 0; i != 5; ++i) {
    if (!sink.Append(Frame(i + 1, (resumed ? 5 : 0) + i, start + 10 + 10 * i)))
      return 13;
  }
  // The OS releases the file lock. No explicit Close or destructor may be
  // necessary for batches already acknowledged by Append to survive restart.
  if (!resumed) std::_Exit(0);
  if (resumed && !sink.Append(Frame(6, 0, 2060, 101))) return 14;
  return sink.Close() ? 0 : 15;
}
}  // namespace

void setUp() {}
void tearDown() {}
int main(int argc, char** argv) {
  if (argc >= 3) return ProcessMode(argv[1], argv[2], argc > 3 ? argv[3] : "");
  UNITY_BEGIN();
  RUN_TEST(test_packed_block_prefix_recovery_preserves_metric_history);
  RUN_TEST(test_native_append_and_reopen_preserve_bytes);
  RUN_TEST(test_native_rejects_invalid_batch_before_writing_prefix);
  RUN_TEST(test_native_repairs_only_an_incomplete_final_record);
  RUN_TEST(test_native_malformed_or_refused_tail_is_not_modified);
  RUN_TEST(test_native_write_owner_is_exclusive);
  RUN_TEST(test_native_shared_sink_serializes_four_writer_batches);
  RUN_TEST(test_short_writes_are_completed_before_acknowledgement);
  RUN_TEST(test_partial_write_error_rolls_back_and_retry_is_exact);
  RUN_TEST(test_zero_write_and_throwing_write_do_not_acknowledge);
  RUN_TEST(test_sync_failure_rolls_back_before_successful_retry);
  RUN_TEST(test_failed_file_callback_retains_exact_stream_bytes);
  RUN_TEST(test_failed_repair_stays_poisoned_until_explicit_close);
  RUN_TEST(test_parent_sync_failure_without_repair_preserves_original_file);
  RUN_TEST(test_visible_malformed_partial_header_is_not_repaired);
  RUN_TEST(test_unconfirmed_rollback_poison_is_sticky);
  RUN_TEST(test_open_read_failure_and_record_limit_never_repair_data);
  RUN_TEST(test_external_size_change_never_gets_overwritten);
  RUN_TEST(
      test_metric_restore_survives_write_and_sync_failures_without_recounting);
  RUN_TEST(test_restart_restores_only_accepted_tags_after_partial_metric_drain);
  RUN_TEST(
      test_file_restore_capacity_and_read_failures_leave_stream_unmodified);
  RUN_TEST(test_restore_error_outcome_survives_another_writer);
  RUN_TEST(test_metric_restart_at_every_frame_byte_preserves_complete_totals);
  RUN_TEST(test_concurrent_restore_retry_and_drain_keep_each_writer_totals);
  return UNITY_END();
}
