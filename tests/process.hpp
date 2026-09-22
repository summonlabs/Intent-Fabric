// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Process helpers used by the process-level tests. These spawn real OS
// processes; they do not run in-process threads pretending to be peers.

#ifndef IFABRIC_TEST_PROCESS_HPP
#define IFABRIC_TEST_PROCESS_HPP

#include "ifabric/core.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ifabric_test {

struct ProcessResult {
  int exit_code = -1;
  std::string output;
};

// Runs a program to completion, capturing stdout and stderr. Arguments are
// passed verbatim; no shell interpretation of the program name occurs.
ProcessResult run_process(const std::string& program, const std::vector<std::string>& args,
                          const std::filesystem::path& working_directory = {});

struct ChildProcess {
  void* handle = nullptr;
  unsigned long pid = 0;
  std::filesystem::path output_path;
  bool reaped = false;

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ~ChildProcess();

  bool running() const;
  void terminate();
  int wait();
  std::string output() const;
};

// Starts a program without waiting for it.
ifabric::Result<ChildProcess> spawn_process(const std::string& program,
                                            const std::vector<std::string>& args,
                                            const std::filesystem::path& output_path);

// A temporary directory that removes itself when it goes out of scope.
class TempDirectory {
 public:
  explicit TempDirectory(const std::string& label);
  ~TempDirectory();
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path child(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// Polls a TCP endpoint until it accepts a connection, or the bounded attempt
// budget is exhausted. Startup synchronisation only: it never waits on the
// behaviour under test.
bool wait_for_port(const std::string& host, std::uint16_t port, unsigned attempts = 400,
                   unsigned delay_ms = 25);

}  // namespace ifabric_test

#endif  // IFABRIC_TEST_PROCESS_HPP
