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
#include "aether-tele/raw/file_native.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ae::tele::raw::file_internal {
namespace {
bool InvalidHandle(std::error_code& error) {
  error = std::make_error_code(std::errc::bad_file_descriptor);
  return false;
}
bool InvalidOffset(std::error_code& error) {
  error = std::make_error_code(std::errc::value_too_large);
  return false;
}
#if defined(_WIN32)
bool WindowsError(std::error_code& error) {
  error = {static_cast<int>(GetLastError()), std::system_category()};
  return false;
}
bool Seek(HANDLE handle, std::uint64_t offset, std::error_code& error) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
    return InvalidOffset(error);
  }
  LARGE_INTEGER distance;
  distance.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, distance, nullptr, FILE_BEGIN)) {
    return WindowsError(error);
  }
  return true;
}
#else
bool PosixError(std::error_code& error) {
  error = {errno, std::system_category()};
  return false;
}
bool Offset(std::uint64_t offset, off_t& value, std::error_code& error) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return InvalidOffset(error);
  }
  value = static_cast<off_t>(offset);
  return true;
}
int OpenDirectory(char const* path) {
  int result;
  do {
    result = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (result < 0 && errno == EINTR);
  return result;
}
int OpenAt(int directory, char const* name, int flags) {
  int result;
  do {
    result = openat(directory, name, flags, 0600);
  } while (result < 0 && errno == EINTR);
  return result;
}
#endif
}  // namespace

NativeFile::~NativeFile() {
  std::error_code ignored;
  Close(ignored);
}

bool NativeFile::Open(std::filesystem::path const& path,
                      std::error_code& error) {
  error.clear();
  if (IsOpen()) {
    error = std::make_error_code(std::errc::device_or_resource_busy);
    return false;
  }
  if (path.empty()) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  if (path.native().find(std::filesystem::path::value_type{}) !=
      std::filesystem::path::string_type::npos) {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return WindowsError(error);
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) {
    WindowsError(error);
    CloseHandle(handle);
    return false;
  }
  if (GetFileType(handle) != FILE_TYPE_DISK ||
      (information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    CloseHandle(handle);
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  handle_ = handle;
  return true;
#else
  // Hold the parent directory handle: openat and fsync then refer to the same
  // directory even if the application's current directory changes later.
  auto parent = path.parent_path();
  if (parent.empty()) parent = ".";
  auto name = path.filename();
  if (name.empty() || name == "." || name == "..") {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  int parent_fd = OpenDirectory(parent.c_str());
  if (parent_fd < 0) return PosixError(error);
  // Nonblocking open prevents a FIFO/device from stalling before fstat rejects
  // it. O_NONBLOCK has no effect on subsequent IO to regular files.
  int const flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
  int fd = OpenAt(parent_fd, name.c_str(), flags | O_CREAT | O_EXCL);
  if (fd < 0 && errno == EEXIST) fd = OpenAt(parent_fd, name.c_str(), flags);
  if (fd < 0) {
    PosixError(error);
    close(parent_fd);
    return false;
  }
  struct stat information {};
  if (fstat(fd, &information) != 0) {
    PosixError(error);
    close(fd);
    close(parent_fd);
    return false;
  }
  if (!S_ISREG(information.st_mode)) {
    close(fd);
    close(parent_fd);
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
  int locked;
  do {
    locked = flock(fd, LOCK_EX | LOCK_NB);
  } while (locked < 0 && errno == EINTR);
  if (locked != 0) {
    PosixError(error);
    close(fd);
    close(parent_fd);
    return false;
  }
  fd_ = fd;
  parent_fd_ = parent_fd;
  // A previous creator may have failed its parent sync, or another opener may
  // have won the lock between our create and flock. Existing does not imply
  // that the directory entry is durable, so sync once on every successful open.
  parent_sync_pending_ = true;
  return true;
#endif
}

bool NativeFile::Size(std::uint64_t& size, std::error_code& error) noexcept {
  error.clear();
  size = 0;
  if (!IsOpen()) return InvalidHandle(error);
#if defined(_WIN32)
  LARGE_INTEGER result{};
  if (!GetFileSizeEx(handle_, &result)) return WindowsError(error);
  if (result.QuadPart < 0) return InvalidOffset(error);
  size = static_cast<std::uint64_t>(result.QuadPart);
#else
  struct stat result {};
  if (fstat(fd_, &result) != 0) return PosixError(error);
  if (result.st_size < 0) return InvalidOffset(error);
  size = static_cast<std::uint64_t>(result.st_size);
#endif
  return true;
}

bool NativeFile::ReadAt(std::uint64_t offset, std::span<std::uint8_t> out,
                        std::size_t& read, std::error_code& error) noexcept {
  error.clear();
  read = 0;
  if (!IsOpen()) return InvalidHandle(error);
#if defined(_WIN32)
  if (!Seek(handle_, offset, error)) return false;
  auto const count = static_cast<DWORD>(std::min<std::size_t>(
      out.size(), std::numeric_limits<DWORD>::max()));
  if (count == 0) return true;
  DWORD result{};
  if (!ReadFile(handle_, out.data(), count, &result, nullptr)) {
    return WindowsError(error);
  }
  read = result;
#else
  off_t position{};
  if (!Offset(offset, position, error)) return false;
  auto const count = std::min<std::size_t>(
      out.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
  if (count == 0) return true;
  ssize_t result;
  do {
    result = pread(fd_, out.data(), count, position);
  } while (result < 0 && errno == EINTR);
  if (result < 0) return PosixError(error);
  read = static_cast<std::size_t>(result);
#endif
  return true;
}

bool NativeFile::WriteAt(std::uint64_t offset,
                         std::span<std::uint8_t const> data,
                         std::size_t& written,
                         std::error_code& error) noexcept {
  error.clear();
  written = 0;
  if (!IsOpen()) return InvalidHandle(error);
#if defined(_WIN32)
  if (!Seek(handle_, offset, error)) return false;
  auto const count = static_cast<DWORD>(std::min<std::size_t>(
      data.size(), std::numeric_limits<DWORD>::max()));
  if (count == 0) return true;
  DWORD result{};
  if (!WriteFile(handle_, data.data(), count, &result, nullptr)) {
    return WindowsError(error);
  }
  written = result;
#else
  off_t position{};
  if (!Offset(offset, position, error)) return false;
  auto const count = std::min<std::size_t>(
      data.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
  if (count == 0) return true;
  ssize_t result;
  do {
    result = pwrite(fd_, data.data(), count, position);
  } while (result < 0 && errno == EINTR);
  if (result < 0) return PosixError(error);
  written = static_cast<std::size_t>(result);
#endif
  return true;
}

bool NativeFile::Truncate(std::uint64_t size,
                          std::error_code& error) noexcept {
  error.clear();
  if (!IsOpen()) return InvalidHandle(error);
#if defined(_WIN32)
  if (!Seek(handle_, size, error)) return false;
  if (!SetEndOfFile(handle_)) return WindowsError(error);
#else
  off_t length{};
  if (!Offset(size, length, error)) return false;
  int result;
  do {
    result = ftruncate(fd_, length);
  } while (result != 0 && errno == EINTR);
  if (result != 0) return PosixError(error);
#endif
  return true;
}

bool NativeFile::Sync(std::error_code& error) noexcept {
  error.clear();
  if (!IsOpen()) return InvalidHandle(error);
#if defined(_WIN32)
  if (!FlushFileBuffers(handle_)) return WindowsError(error);
#else
  int result;
  do {
    result = fsync(fd_);
  } while (result != 0 && errno == EINTR);
  if (result != 0) return PosixError(error);
#endif
  return true;
}

bool NativeFile::SyncParent(std::error_code& error) noexcept {
  error.clear();
  if (!IsOpen()) return InvalidHandle(error);
#if !defined(_WIN32)
  if (parent_sync_pending_) {
    int result;
    do {
      result = fsync(parent_fd_);
    } while (result != 0 && errno == EINTR);
    if (result != 0) return PosixError(error);
    parent_sync_pending_ = false;
  }
#endif
  return true;
}

bool NativeFile::Close(std::error_code& error) noexcept {
  error.clear();
#if defined(_WIN32)
  if (handle_ == nullptr) return true;
  auto const handle = handle_;
  handle_ = nullptr;
  if (!CloseHandle(handle)) return WindowsError(error);
#else
  auto const fd = fd_;
  auto const parent_fd = parent_fd_;
  fd_ = parent_fd_ = -1;
  parent_sync_pending_ = false;
  // Do not retry close after EINTR: on Linux the descriptor is already released
  // and another thread may reuse its integer value before a retry.
  if (fd >= 0 && close(fd) != 0) PosixError(error);
  if (parent_fd >= 0 && close(parent_fd) != 0 && !error) PosixError(error);
  if (error) return false;
#endif
  return true;
}

bool NativeFile::IsOpen() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

}  // namespace ae::tele::raw::file_internal
