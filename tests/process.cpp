// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "process.hpp"

#include "ifabric/net.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ifabric_test {

using ifabric::Error;
using ifabric::ErrorCode;
using ifabric::Result;

namespace {

std::string quote(const std::string& value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') {
      out.append("\\\"");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}
#endif

}  // namespace

ProcessResult run_process(const std::string& program, const std::vector<std::string>& args,
                          const std::filesystem::path& working_directory) {
  ProcessResult result;
#if defined(_WIN32)
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      ("ifabric-run-" + std::to_string(::ifabric::current_process_id()) + "-" +
       std::to_string(::GetTickCount64()) + ".log");
  std::string command = quote(program);
  for (const auto& arg : args) {
    command.push_back(' ');
    command.append(quote(arg));
  }
  const std::wstring wide_command = widen(command);
  const std::wstring wide_output = widen(output_path.string());
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE file = ::CreateFileW(wide_output.c_str(), GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    result.exit_code = -1;
    result.output = "cannot create the captured output file";
    return result;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION information{};
  std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
  mutable_command.push_back(L'\0');
  const std::wstring wide_directory =
      widen(working_directory.empty() ? std::string() : working_directory.string());
  const BOOL started = ::CreateProcessW(
      nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
      wide_directory.empty() ? nullptr : wide_directory.c_str(), &startup, &information);
  if (!started) {
    ::CloseHandle(file);
    (void)std::filesystem::remove(output_path);
    result.exit_code = -1;
    result.output = "cannot start process";
    return result;
  }
  ::CloseHandle(information.hThread);
  ::WaitForSingleObject(information.hProcess, INFINITE);
  DWORD code = 1;
  ::GetExitCodeProcess(information.hProcess, &code);
  ::CloseHandle(information.hProcess);
  result.exit_code = static_cast<int>(code);
  ::CloseHandle(file);
  {
    std::ifstream input(output_path, std::ios::binary);
    if (input) {
      std::ostringstream buffer;
      buffer << input.rdbuf();
      result.output = buffer.str();
    }
  }
  std::error_code error;
  std::filesystem::remove(output_path, error);
#else
  static std::mt19937_64 engine{0x5eed1234ull};
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      ("ifabric-run-" + std::to_string(::ifabric::current_process_id()) + "-" +
       std::to_string(engine()) + ".log");
  std::string command = quote(program);
  for (const auto& arg : args) {
    command.push_back(' ');
    command.append(quote(arg));
  }
  command.append(" > ");
  command.append(quote(output_path.string()));
  command.append(" 2>&1");
  if (!working_directory.empty()) {
    command = "cd " + quote(working_directory.string()) + " && " + command;
  }
  const int status = std::system(command.c_str());
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  {
    std::ifstream input(output_path, std::ios::binary);
    if (input) {
      std::ostringstream buffer;
      buffer << input.rdbuf();
      result.output = buffer.str();
    }
  }
  std::error_code error;
  std::filesystem::remove(output_path, error);
#endif
  return result;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle(other.handle),
      pid(other.pid),
      output_path(std::move(other.output_path)),
      reaped(other.reaped) {
  other.handle = nullptr;
  other.pid = 0;
  other.reaped = true;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    terminate();
    handle = other.handle;
    pid = other.pid;
    output_path = std::move(other.output_path);
    reaped = other.reaped;
    other.handle = nullptr;
    other.pid = 0;
    other.reaped = true;
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  terminate();
  if (!reaped && handle != nullptr) {
    (void)wait();
  }
}

#if defined(_WIN32)
bool ChildProcess::running() const {
  if (handle == nullptr) {
    return false;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(handle), &code) == 0) {
    return false;
  }
  return code == STILL_ACTIVE;
}

void ChildProcess::terminate() {
  if (handle == nullptr) {
    return;
  }
  if (running()) {
    ::TerminateProcess(static_cast<HANDLE>(handle), 137);
  }
}

int ChildProcess::wait() {
  if (handle == nullptr) {
    return -1;
  }
  ::WaitForSingleObject(static_cast<HANDLE>(handle), INFINITE);
  DWORD code = 1;
  ::GetExitCodeProcess(static_cast<HANDLE>(handle), &code);
  ::CloseHandle(static_cast<HANDLE>(handle));
  handle = nullptr;
  reaped = true;
  return static_cast<int>(code);
}
#else
bool ChildProcess::running() const {
  if (pid == 0) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  return result == 0;
}

void ChildProcess::terminate() {
  if (pid != 0 && running()) {
    ::kill(static_cast<pid_t>(pid), SIGKILL);
  }
}

int ChildProcess::wait() {
  if (pid == 0) {
    return -1;
  }
  int status = 0;
  (void)::waitpid(static_cast<pid_t>(pid), &status, 0);
  reaped = true;
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
  pid = 0;
  return code;
}
#endif

std::string ChildProcess::output() const {
  std::ifstream input(output_path, std::ios::binary);
  if (!input) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

Result<ChildProcess> spawn_process(const std::string& program,
                                   const std::vector<std::string>& args,
                                   const std::filesystem::path& output_path) {
  ChildProcess child;
  child.output_path = output_path;
#if defined(_WIN32)
  std::string command = quote(program);
  for (const auto& arg : args) {
    command.push_back(' ');
    command.append(quote(arg));
  }
  const std::wstring wide_command = widen(command);
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const std::wstring wide_output = widen(output_path.string());
  HANDLE file = ::CreateFileW(wide_output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError, "cannot create the child output file", output_path.string());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION information{};
  std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
  mutable_command.push_back(L'\0');
  const BOOL started = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  ::CloseHandle(file);
  if (!started) {
    return Error(ErrorCode::Internal, "cannot start child process", program);
  }
  ::CloseHandle(information.hThread);
  child.handle = information.hProcess;
  child.pid = information.dwProcessId;
  return child;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    return Error(ErrorCode::Internal, "fork failed", program);
  }
  if (pid == 0) {
    const int fd = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      (void)::dup2(fd, STDOUT_FILENO);
      (void)::dup2(fd, STDERR_FILENO);
      (void)::close(fd);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);
  }
  child.pid = static_cast<unsigned long>(pid);
  return child;
#endif
}

TempDirectory::TempDirectory(const std::string& label) {
  static std::mt19937_64 engine{0xc0ffee01ull};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  path_ = std::filesystem::temp_directory_path() /
          ("ifabric-" + label + "-" + std::to_string(::ifabric::current_process_id()) + "-" +
           std::to_string(stamp) + "-" + std::to_string(engine()));
  std::error_code error;
  std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

bool wait_for_port(const std::string& host, std::uint16_t port, unsigned attempts,
                   unsigned delay_ms) {
  for (unsigned attempt = 0; attempt < attempts; ++attempt) {
    const auto socket = ifabric::net::connect_tcp(host, port, 200);
    if (socket) {
      (void)ifabric::net::close_socket(socket.value());
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
  return false;
}

}  // namespace ifabric_test
