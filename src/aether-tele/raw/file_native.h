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
#ifndef AETHER_TELE_RAW_FILE_NATIVE_H_
#define AETHER_TELE_RAW_FILE_NATIVE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

namespace ae::tele::raw::file_internal {

// Native regular-file IO. Calls are serialized by the owning file drain.
// The open handle is the lock identity; do not unlink/replace a live log.
// POSIX locks are advisory and cannot protect against non-cooperating writers.
class NativeFile {
 public:
#if defined(_WIN32)
  // Win32 has no portable directory-fsync equivalent. Sync() acknowledges file
  // contents and length via FlushFileBuffers; newly created name durability is
  // a filesystem/OS boundary, not a guarantee supplied by SyncParent().
  static constexpr bool kDirectorySyncSupported = false;
#else
  static constexpr bool kDirectorySyncSupported = true;
#endif

  NativeFile() = default;
  ~NativeFile();
  NativeFile(NativeFile const&) = delete;
  NativeFile& operator=(NativeFile const&) = delete;
  NativeFile(NativeFile&&) = delete;
  NativeFile& operator=(NativeFile&&) = delete;

  bool Open(std::filesystem::path const& path, std::error_code& error);
  bool Size(std::uint64_t& size, std::error_code& error) noexcept;
  bool ReadAt(std::uint64_t offset, std::span<std::uint8_t> out,
              std::size_t& read, std::error_code& error) noexcept;
  bool WriteAt(std::uint64_t offset, std::span<std::uint8_t const> data,
               std::size_t& written, std::error_code& error) noexcept;
  bool Truncate(std::uint64_t size, std::error_code& error) noexcept;
  bool Sync(std::error_code& error) noexcept;
  bool SyncParent(std::error_code& error) noexcept;
  bool Close(std::error_code& error) noexcept;
  bool IsOpen() const noexcept;

 private:
#if defined(_WIN32)
  void* handle_{};
#else
  int fd_ = -1;
  int parent_fd_ = -1;
  bool parent_sync_pending_{};
#endif
};

}  // namespace ae::tele::raw::file_internal
#endif  // AETHER_TELE_RAW_FILE_NATIVE_H_
