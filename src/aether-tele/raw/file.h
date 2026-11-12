/*
 * Copyright 2026 Aethernet Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at https://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#ifndef AETHER_TELE_RAW_FILE_H_
#define AETHER_TELE_RAW_FILE_H_

// Host-side durable storage. Kept separate from the allocation-free raw API.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <system_error>
#include <vector>

#include "aether-tele/raw/decode.h"
#include "aether-tele/raw/file_native.h"

namespace ae::tele::raw {
enum class FileState { kClosed, kReady, kPoisoned };
enum class FileError {
  kNone,
  kNotOpen,
  kAlreadyOpen,
  kInvalidOptions,
  kOpen,
  kRead,
  kSize,
  kInvalidInput,
  kRecordTooLarge,
  kMalformedFile,
  kIncompleteFile,
  kWrite,
  kSync,
  kDirectorySync,
  kRollbackFailed,
  kPoisoned,
  kClose,
  kMemory,
  kExternalChange,
  kMetricCapacity,
  kRestoreState,
};
struct FileStatus {
  FileError error{FileError::kNone};
  std::error_code io_error{};
  std::error_code rollback_error{};
  std::uint64_t offset{};
};
struct FileOptions {
  std::uint32_t max_record_bytes = wire::BlockSizeCodec::kUpper;
  bool repair_incomplete_tail = true;
};
struct RecoveryInfo {
  std::uint64_t original_size{};
  std::uint64_t valid_size{};
  std::uint64_t truncated_bytes{};
  bool directory_sync_supported{};
};

// Backend injection is for platform adapters/fault tests. It must outlive this
// object, must initially be closed, and is exclusively owned by this adapter.
// It must not be accessed directly while the adapter exists. Backend
// operations report error_code; unexpected exceptions are contained as IO
// errors.
template <typename Backend>
class BasicFileDrain {
 public:
  explicit BasicFileDrain(Backend& backend) noexcept : backend_(backend) {}
  ~BasicFileDrain() { Close(); }
  BasicFileDrain(BasicFileDrain const&) = delete;
  BasicFileDrain& operator=(BasicFileDrain const&) = delete;

  bool Open(std::filesystem::path const& path,
            FileOptions options = {}) noexcept {
    std::lock_guard lock(mutex_);
    if (state_ == FileState::kPoisoned) return false;
    if (state_ != FileState::kClosed) return Fail(FileError::kAlreadyOpen);
    if (options.max_record_bytes < kMinBlockSize)
      return Fail(FileError::kInvalidOptions);
    options_ = options;
    recovery_ = {};
    recovery_.directory_sync_supported = Backend::kDirectorySyncSupported;
    size_ = 0;
    std::error_code error;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!backend_.Open(path, error))
        return OpenFailure(FileError::kOpen, error);
      if (!backend_.Size(recovery_.original_size, error))
        return OpenFailure(FileError::kSize, error);
      if (!Scan(error)) {
        CloseBackend();  // Scan never mutates the existing file.
        return false;
      }
      if (recovery_.valid_size != recovery_.original_size) {
        if (!options_.repair_incomplete_tail)
          return OpenFailure(FileError::kIncompleteFile, {},
                             recovery_.valid_size);
        // Any failure after starting repair leaves durability uncertain. Keep
        // the lock until Close; this object stays poisoned permanently. The
        // owner must reconcile and create a fresh adapter before any retry.
        state_ = FileState::kPoisoned;
        if (!backend_.Truncate(recovery_.valid_size, error))
          return Fail(FileError::kWrite, error, recovery_.valid_size);
      }
      if (!backend_.Sync(error)) {
        if (state_ == FileState::kPoisoned)
          return Fail(FileError::kSync, error, recovery_.valid_size);
        return OpenFailure(FileError::kSync, error, recovery_.valid_size);
      }
      if (!backend_.SyncParent(error)) {
        if (state_ == FileState::kPoisoned)
          return Fail(FileError::kDirectorySync, error, recovery_.valid_size);
        return OpenFailure(FileError::kDirectorySync, error,
                           recovery_.valid_size);
      }
      size_ = recovery_.valid_size;
      recovery_.truncated_bytes =
          recovery_.original_size - recovery_.valid_size;
      state_ = FileState::kReady;
      status_ = {};
      return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (std::bad_alloc const&) {
      if (state_ != FileState::kPoisoned) CloseBackend();
      return Fail(FileError::kMemory,
                  std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
      if (state_ != FileState::kPoisoned) CloseBackend();
      return Fail(FileError::kOpen, std::make_error_code(std::errc::io_error));
    }
#endif
  }

  // Call on each new, quiescent Stream before starting its writer. A stable
  // stream ID selects its history; other writers' totals are not imported.
  // Scanning/aggregation finishes before touching the Stream, so IO/capacity
  // failures leave it available for a retry. No bytes are appended here. The
  // optional outcome belongs to this call, even if another writer changes the
  // adapter's last status immediately after the mutex is released.
  template <typename StreamT>
  bool RestoreMetrics(StreamT& stream, FileStatus* outcome = nullptr) noexcept {
    std::lock_guard lock(mutex_);
    auto finish = [&](bool ok) {
      if (outcome) *outcome = status_;
      return ok;
    };
    auto fail = [&](FileError code, std::error_code error = {},
                    std::uint64_t offset = 0) {
      return finish(Fail(code, error, offset));
    };
    if (state_ == FileState::kPoisoned) {
      if (outcome) {
        *outcome = status_;
        outcome->error = FileError::kPoisoned;
      }
      return false;
    }
    if (state_ != FileState::kReady) return fail(FileError::kNotOpen);
    std::error_code error;
    std::uint64_t offset = 0;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      std::uint64_t actual{};
      if (!backend_.Size(actual, error)) return fail(FileError::kSize, error);
      if (actual != size_) {
        state_ = FileState::kPoisoned;
        return fail(FileError::kExternalChange);
      }
      std::vector<MetricTotal> totals;
      std::vector<std::uint8_t> frame;
      while (offset < size_) {
        std::uint64_t declared{};
        bool incomplete{};
        if (!ReadBlockSize(offset, size_ - offset, declared, incomplete, error))
          return finish(false);
        if (incomplete || declared > size_ - offset)
          return fail(FileError::kMalformedFile, {}, offset);
        frame.resize(declared);
        if (!ReadExact(offset, frame, error))
          return fail(FileError::kRead, error, offset);
        auto const decoded = Decode(frame);
        if (decoded.status != DecodeStatus::kOk || decoded.records.empty())
          return fail(FileError::kMalformedFile, {}, offset);
        for (auto const& record : decoded.records) {
          if (record.metric && record.stream == stream.id()) {
            auto found = std::find_if(
                totals.begin(), totals.end(),
                [&](auto const& total) { return total.index == record.index; });
            if (found == totals.end()) {
              if (totals.size() == StreamT::kMetricCapacity)
                return fail(FileError::kMetricCapacity, {}, offset);
              totals.push_back({record.index, {}});
              AccumulateMetric(totals.back().metric, *record.metric);
            } else {
              AccumulateMetric(found->metric, *record.metric);
            }
          }
        }
        offset += declared;
      }
      if (!stream.RestoreMetrics(std::span<MetricTotal const>{totals}))
        return fail(FileError::kRestoreState);
      status_ = {};
      return finish(true);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (std::bad_alloc const&) {
      return fail(FileError::kMemory,
                  std::make_error_code(std::errc::not_enough_memory), offset);
    } catch (...) {
      return fail(FileError::kRead, std::make_error_code(std::errc::io_error),
                  offset);
    }
#endif
  }

  // Serialize complete batches from different owner streams. The optional
  // outcome is copied under the mutex, so another caller cannot overwrite this
  // operation's diagnostic before its owner receives it.
  bool Append(std::span<std::uint8_t const> bytes,
              FileStatus* outcome = nullptr) noexcept {
    std::lock_guard lock(mutex_);
    auto finish = [&](bool ok) {
      if (outcome) *outcome = status_;
      return ok;
    };
    if (state_ == FileState::kPoisoned) {
      if (outcome) {
        *outcome = status_;
        outcome->error = FileError::kPoisoned;
      }
      return false;
    }
    if (state_ != FileState::kReady) return finish(Fail(FileError::kNotOpen));
    auto const checkpoint = size_;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!Validate(bytes)) return finish(false);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (std::bad_alloc const&) {
      return finish(Fail(FileError::kMemory,
                         std::make_error_code(std::errc::not_enough_memory),
                         checkpoint));
    } catch (...) {
      return finish(Fail(FileError::kInvalidInput, {}, checkpoint));
    }
#endif
    if (bytes.empty()) {
      status_ = {};
      return finish(true);
    }
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - checkpoint)
      return finish(Fail(FileError::kInvalidInput, {}, checkpoint));
    std::error_code error;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      std::uint64_t actual{};
      if (!backend_.Size(actual, error))
        return finish(Fail(FileError::kSize, error, checkpoint));
      if (actual != checkpoint) {
        state_ = FileState::kPoisoned;
        return finish(Fail(FileError::kExternalChange, {}, checkpoint));
      }
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      return finish(Fail(FileError::kSize,
                         std::make_error_code(std::errc::io_error),
                         checkpoint));
    }
#endif
    FileError failure = FileError::kWrite;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      std::size_t done = 0;
      while (done < bytes.size()) {
        std::size_t written{};
        error.clear();
        if (!backend_.WriteAt(checkpoint + done, bytes.subspan(done), written,
                              error) ||
            written == 0 || written > bytes.size() - done) {
          if (!error) error = std::make_error_code(std::errc::io_error);
          return finish(Rollback(checkpoint, failure, error));
        }
        done += written;
      }
      failure = FileError::kSync;
      error.clear();
      if (!backend_.Sync(error))
        return finish(Rollback(checkpoint, failure, error));
      size_ = checkpoint + bytes.size();
      status_ = {};
      return finish(true);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      return finish(Rollback(checkpoint, failure,
                             std::make_error_code(std::errc::io_error)));
    }
#endif
  }

  bool Close() noexcept {
    std::lock_guard lock(mutex_);
    std::error_code error;
    bool success = false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      if (!backend_.IsOpen()) return state_ != FileState::kPoisoned;
      success = backend_.Close(error);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      error = std::make_error_code(std::errc::io_error);
      if (state_ != FileState::kPoisoned) {
        state_ = FileState::kPoisoned;
        return Fail(FileError::kClose, error, size_);
      }
    }
#endif
    if (state_ == FileState::kPoisoned) return success;
    state_ = FileState::kClosed;
    if (!success) return Fail(FileError::kClose, error, size_);
    status_ = {};
    return true;
  }
  static bool Callback(void* context,
                       std::span<std::uint8_t const> bytes) noexcept {
    return context != nullptr &&
           static_cast<BasicFileDrain*>(context)->Append(bytes);
  }
  FileState state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }
  FileStatus status() const {
    std::lock_guard lock(mutex_);
    return status_;
  }
  RecoveryInfo recovery() const {
    std::lock_guard lock(mutex_);
    return recovery_;
  }
  std::uint64_t size() const {
    std::lock_guard lock(mutex_);
    return size_;
  }

 private:
  bool Fail(FileError code, std::error_code error = {},
            std::uint64_t offset = 0) noexcept {
    status_ = {code, error, {}, offset};
    return false;
  }
  void CloseBackend() noexcept {
    std::error_code ignored;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      backend_.Close(ignored);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
    }
#endif
    state_ = FileState::kClosed;
  }
  bool OpenFailure(FileError code, std::error_code error = {},
                   std::uint64_t offset = 0) noexcept {
    CloseBackend();
    return Fail(code, error, offset);
  }
  bool ReadExact(std::uint64_t offset, std::span<std::uint8_t> bytes,
                 std::error_code& error) {
    std::size_t done = 0;
    while (done < bytes.size()) {
      std::size_t read{};
      if (!backend_.ReadAt(offset + done, bytes.subspan(done), read, error) ||
          read == 0 || read > bytes.size() - done) {
        if (!error) error = std::make_error_code(std::errc::io_error);
        return false;
      }
      done += read;
    }
    return true;
  }
  bool ReadBlockSize(std::uint64_t offset, std::uint64_t remaining,
                     std::uint64_t& declared, bool& incomplete,
                     std::error_code& error) {
    std::array<std::uint8_t, wire::BlockSizeCodec::kMaxWireBytes> prefix{};
    auto const available = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, prefix.size()));
    if (!ReadExact(offset, std::span{prefix}.first(available), error))
      return Fail(FileError::kRead, error, offset);
    std::size_t cursor = 0;
    auto const status = wire::Read<wire::BlockSizeCodec>(
        std::span{prefix}.first(available), cursor, declared);
    incomplete = status == wire::ReadStatus::kTruncated;
    if (incomplete) return true;
    if (status != wire::ReadStatus::kOk || declared < kMinBlockSize ||
        declared < cursor)
      return Fail(FileError::kMalformedFile, {}, offset);
    if (declared > options_.max_record_bytes)
      return Fail(FileError::kRecordTooLarge, {}, offset);
    return true;
  }
  bool Scan(std::error_code& error) {
    std::vector<std::uint8_t> frame;
    while (recovery_.valid_size < recovery_.original_size) {
      auto const offset = recovery_.valid_size;
      auto const remaining = recovery_.original_size - offset;
      std::uint64_t declared{};
      bool incomplete{};
      if (!ReadBlockSize(offset, remaining, declared, incomplete, error))
        return false;
      if (incomplete) return true;
      auto const available = static_cast<std::size_t>(
          std::min<std::uint64_t>(declared, remaining));
      frame.resize(available);
      if (!ReadExact(offset, frame, error))
        return Fail(FileError::kRead, error, offset);
      auto decoded = Decode(frame);
      if (decoded.status == DecodeStatus::kMalformed)
        return Fail(FileError::kMalformedFile, {}, offset);
      if (decoded.status == DecodeStatus::kTruncated) return true;
      if (decoded.valid_bytes != declared || decoded.records.empty())
        return Fail(FileError::kMalformedFile, {}, offset);
      recovery_.valid_size += declared;
    }
    return true;
  }
  bool Validate(std::span<std::uint8_t const> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto const remaining = bytes.subspan(offset);
      std::size_t cursor = 0;
      std::uint64_t declared{};
      if (wire::Read<wire::BlockSizeCodec>(remaining, cursor, declared) !=
          wire::ReadStatus::kOk)
        return Fail(FileError::kInvalidInput, {}, size_);
      if (declared > options_.max_record_bytes)
        return Fail(FileError::kRecordTooLarge, {}, size_);
      if (declared < kMinBlockSize || declared < cursor ||
          declared > remaining.size())
        return Fail(FileError::kInvalidInput, {}, size_);
      auto decoded = Decode(remaining.first(declared));
      if (decoded.status != DecodeStatus::kOk || decoded.records.empty())
        return Fail(FileError::kInvalidInput, {}, size_);
      offset += declared;
    }
    return true;
  }
  bool Rollback(std::uint64_t checkpoint, FileError original,
                std::error_code error) noexcept {
    std::error_code rollback;
    bool restored = false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
      restored =
          backend_.Truncate(checkpoint, rollback) && backend_.Sync(rollback);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
      rollback = std::make_error_code(std::errc::io_error);
    }
#endif
    size_ = checkpoint;
    if (!restored) {
      state_ = FileState::kPoisoned;
      status_ = {FileError::kRollbackFailed, error, rollback, checkpoint};
      return false;
    }
    status_ = {original, error, {}, checkpoint};
    return false;
  }

  Backend& backend_;
  mutable std::mutex mutex_;
  FileState state_{FileState::kClosed};
  FileOptions options_{};
  FileStatus status_{};
  RecoveryInfo recovery_{};
  std::uint64_t size_{};
};

// Convenient native adapter. Keep this object alive until all writer threads
// have stopped using its callback. One shared instance safely serializes
// batches.
class FileDrain {
 public:
  template <typename StreamT>
  bool RestoreMetrics(StreamT& stream, FileStatus* outcome = nullptr) noexcept {
    return drain_.RestoreMetrics(stream, outcome);
  }
  bool Open(std::filesystem::path const& path,
            FileOptions options = {}) noexcept {
    return drain_.Open(path, options);
  }
  bool Append(std::span<std::uint8_t const> bytes,
              FileStatus* outcome = nullptr) noexcept {
    return drain_.Append(bytes, outcome);
  }
  bool Close() noexcept { return drain_.Close(); }
  FileState state() const { return drain_.state(); }
  FileStatus status() const { return drain_.status(); }
  RecoveryInfo recovery() const { return drain_.recovery(); }
  std::uint64_t size() const { return drain_.size(); }
  static bool Callback(void* context,
                       std::span<std::uint8_t const> bytes) noexcept {
    return context != nullptr &&
           static_cast<FileDrain*>(context)->Append(bytes);
  }

 private:
  file_internal::NativeFile native_;
  BasicFileDrain<file_internal::NativeFile> drain_{native_};
};
}  // namespace ae::tele::raw
#endif  // AETHER_TELE_RAW_FILE_H_
