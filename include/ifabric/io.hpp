// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable file primitives: bounded reads, atomic replace with an optional
// previous-version backup, fsync, directory listing and an exclusive
// process-scoped store lock.

#ifndef IFABRIC_IO_HPP
#define IFABRIC_IO_HPP

#include "ifabric/core.hpp"

#include <filesystem>
#include <memory>

namespace ifabric {

using Path = std::filesystem::path;

bool file_exists(const Path& path);
bool directory_exists(const Path& path);
Error ensure_directory(const Path& path);
Error remove_file(const Path& path);
Result<std::uint64_t> file_size(const Path& path);

// Reads at most max_bytes; a larger file is rejected with LimitExceeded before
// any allocation proportional to its size is made.
Result<std::string> read_file(const Path& path, std::size_t max_bytes);

struct WriteOptions {
  bool fsync_data = true;
  bool keep_backup = false;
};

// write -> fsync -> atomic replace. The target is either the previous content
// or the new content, never a mixture.
Error write_file_atomic(const Path& path, std::string_view bytes, const WriteOptions& options);

Result<std::vector<Path>> list_directory(const Path& path);
Result<std::vector<Path>> list_files(const Path& path, std::string_view extension);

Error sync_directory(const Path& path);

// A store lock is held for the lifetime of the object and is released by the
// operating system even if the process dies.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  static Result<std::unique_ptr<FileLock>> acquire(const Path& path);

  bool held() const noexcept;
  void release();

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace ifabric

#endif  // IFABRIC_IO_HPP
