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

#ifndef AETHER_TELE_SPACE_JOURNAL_H_
#define AETHER_TELE_SPACE_JOURNAL_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "aether-tele/space/crc.h"

namespace ae::tele::space {

inline constexpr std::uint8_t kJournalMagic[4] = {'A', 'T', 'J', '1'};
inline constexpr std::uint8_t kBatchMagic[4] = {'A', 'T', 'B', '1'};

struct JournalBatch {
  std::vector<std::uint8_t> blob;
  bool truncated{false};
  bool corrupt{false};
};

struct JournalFile {
  std::uint32_t sequence{};
  std::vector<JournalBatch> batches;
  bool truncated_tail{false};
};

inline void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  std::uint8_t buf[4];
  WriteU32LE(buf, value);
  out.insert(out.end(), buf, buf + 4);
}

inline std::vector<std::uint8_t> EncodeBatch(
    std::span<std::uint8_t const> blob) {
  Crc32Acc crc;
  crc.FeedBytes(blob.data(), blob.size());
  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kBatchMagic), std::end(kBatchMagic));
  AppendU32(out, static_cast<std::uint32_t>(blob.size()));
  AppendU32(out, crc.Finish());
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

inline std::vector<std::uint8_t> EncodeJournalHeader(std::uint32_t sequence) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(kJournalMagic), std::end(kJournalMagic));
  AppendU32(out, sequence);
  return out;
}

inline JournalFile DecodeJournal(std::span<std::uint8_t const> bytes) {
  JournalFile file;
  if (bytes.size() < 8) {
    file.truncated_tail = !bytes.empty();
    return file;
  }
  if (bytes[0] != kJournalMagic[0] || bytes[1] != kJournalMagic[1] ||
      bytes[2] != kJournalMagic[2] || bytes[3] != kJournalMagic[3]) {
    file.truncated_tail = true;
    return file;
  }
  file.sequence = ReadU32LE(bytes.data() + 4);
  std::size_t off = 8;
  while (off < bytes.size()) {
    auto const remain = bytes.size() - off;
    if (remain < 12) {
      file.truncated_tail = true;
      break;
    }
    if (bytes[off] != kBatchMagic[0] || bytes[off + 1] != kBatchMagic[1] ||
        bytes[off + 2] != kBatchMagic[2] || bytes[off + 3] != kBatchMagic[3]) {
      file.truncated_tail = true;
      break;
    }
    auto const size = ReadU32LE(bytes.data() + off + 4);
    auto const crc = ReadU32LE(bytes.data() + off + 8);
    if (remain < 12 + size) {
      file.truncated_tail = true;
      JournalBatch batch;
      batch.truncated = true;
      file.batches.push_back(std::move(batch));
      break;
    }
    JournalBatch batch;
    batch.blob.assign(bytes.data() + off + 12,
                      bytes.data() + off + 12 + size);
    Crc32Acc acc;
    acc.FeedBytes(batch.blob.data(), batch.blob.size());
    if (acc.Finish() != crc) {
      batch.corrupt = true;
    }
    file.batches.push_back(std::move(batch));
    off += 12 + size;
  }
  return file;
}

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_JOURNAL_H_
