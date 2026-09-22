// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric/io.hpp"

#include <cstdio>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ifabric {

namespace {

FILE* open_read(const Path& path) {
#if defined(_WIN32)
  FILE* stream = nullptr;
  if (::_wfopen_s(&stream, path.c_str(), L"rb") != 0) {
    return nullptr;
  }
  return stream;
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

FILE* open_write(const Path& path) {
#if defined(_WIN32)
  FILE* stream = nullptr;
  if (::_wfopen_s(&stream, path.c_str(), L"wb") != 0) {
    return nullptr;
  }
  return stream;
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

void sync_stream(FILE* stream) {
  if (stream == nullptr) {
    return;
  }
  (void)std::fflush(stream);
#if defined(_WIN32)
  (void)::_commit(::_fileno(stream));
#else
  (void)::fsync(::fileno(stream));
#endif
}

Error write_file_plain(const Path& path, std::string_view bytes, bool fsync_data) {
  FILE* stream = open_write(path);
  if (stream == nullptr) {
    return Error(ErrorCode::IoError, "cannot open file for writing", path.string());
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), stream);
    if (written != bytes.size()) {
      (void)std::fclose(stream);
      return Error(ErrorCode::IoError, "short write", path.string());
    }
  }
  if (fsync_data) {
    sync_stream(stream);
  }
  if (std::fclose(stream) != 0) {
    return Error(ErrorCode::IoError, "cannot close file after writing", path.string());
  }
  return Error();
}

std::string temp_sibling(const Path& path) {
  return path.string() + ".tmp";
}

std::string backup_sibling(const Path& path) {
  return path.string() + ".bak";
}

}  // namespace

bool file_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

bool directory_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error);
}

Error ensure_directory(const Path& path) {
  std::error_code error;
  if (std::filesystem::is_directory(path, error)) {
    return Error();
  }
  if (!std::filesystem::create_directories(path, error) && error) {
    return Error(ErrorCode::IoError, "cannot create directory", path.string() + ": " + error.message());
  }
  return Error();
}

Error remove_file(const Path& path) {
  std::error_code error;
  (void)std::filesystem::remove(path, error);
  return Error();
}

Result<std::uint64_t> file_size(const Path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot stat file", path.string() + ": " + error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::string> read_file(const Path& path, std::size_t max_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Error(ErrorCode::StoreNotFound, "cannot read file size", path.string());
  }
  if (size > static_cast<std::uintmax_t>(max_bytes)) {
    return Error(ErrorCode::LimitExceeded, "file exceeds the configured byte bound",
                 path.string() + ": " + std::to_string(size) + " > " + std::to_string(max_bytes));
  }
  FILE* stream = open_read(path);
  if (stream == nullptr) {
    return Error(ErrorCode::IoError, "cannot open file for reading", path.string());
  }
  std::string out;
  out.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < out.size()) {
    const std::size_t read = std::fread(out.data() + offset, 1, out.size() - offset, stream);
    if (read == 0) {
      break;
    }
    offset += read;
  }
  (void)std::fclose(stream);
  if (offset != out.size()) {
    return Error(ErrorCode::IoError, "short read", path.string());
  }
  return out;
}

Error write_file_atomic(const Path& path, std::string_view bytes, const WriteOptions& options) {
  const Path temporary(temp_sibling(path));
  Error error = write_file_plain(temporary, bytes, options.fsync_data);
  if (!error.ok()) {
    (void)remove_file(temporary);
    return error;
  }
  if (options.keep_backup && file_exists(path)) {
    const auto previous = read_file(path, kMaxRecordBytes + 4096u);
    if (previous.ok()) {
      const Path backup(backup_sibling(path));
      const Error backup_error = write_file_plain(backup, previous.value(), options.fsync_data);
      if (!backup_error.ok()) {
        (void)remove_file(temporary);
        return backup_error;
      }
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    (void)remove_file(temporary);
    return Error(ErrorCode::AtomicReplaceFailed, "atomic replace failed",
                 path.string() + ": " + rename_error.message());
  }
  if (options.fsync_data) {
    (void)sync_directory(path.parent_path());
  }
  return Error();
}

Result<std::vector<Path>> list_directory(const Path& path) {
  std::error_code error;
  std::vector<Path> out;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return Error(ErrorCode::IoError, "cannot list directory", path.string() + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  if (out.size() > kMaxCollectionMembers) {
    return Error(ErrorCode::LimitExceeded, "directory contains too many entries", path.string());
  }
  return out;
}

Result<std::vector<Path>> list_files(const Path& path, std::string_view extension) {
  const auto entries = list_directory(path);
  if (!entries) {
    return entries.error();
  }
  std::vector<Path> out;
  for (const auto& entry : entries.value()) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(entry, error)) {
      continue;
    }
    if (!extension.empty() && entry.extension().string() != extension) {
      continue;
    }
    out.push_back(entry);
  }
  return out;
}

Error sync_directory(const Path& path) {
#if defined(_WIN32)
  (void)path;
  return Error();
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Error();
  }
  (void)::fsync(fd);
  (void)::close(fd);
  return Error();
#endif
}

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept {
#if defined(_WIN32)
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  fd_ = other.fd_;
  other.fd_ = -1;
#endif
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }
  return *this;
}

bool FileLock::held() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

void FileLock::release() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    (void)::flock(fd_, LOCK_UN);
    (void)::close(fd_);
    fd_ = -1;
  }
#endif
}

Result<std::unique_ptr<FileLock>> FileLock::acquire(const Path& path) {
  auto lock = std::make_unique<FileLock>();
#if defined(_WIN32)
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return Error(ErrorCode::StoreLockBusy, "another process holds the store lock",
                   path.string());
    }
    return Error(ErrorCode::IoError, "cannot open the store lock file", path.string());
  }
  lock->handle_ = handle;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return Error(ErrorCode::IoError, "cannot open the store lock file", path.string());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    (void)::close(fd);
    return Error(ErrorCode::StoreLockBusy, "another process holds the store lock", path.string());
  }
  lock->fd_ = fd;
#endif
  return lock;
}

}  // namespace ifabric
