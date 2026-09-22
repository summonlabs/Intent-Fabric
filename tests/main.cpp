// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ifabric_test.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ifabric_test {

namespace {
std::size_t g_assertions = 0;
std::size_t g_failures = 0;
std::string g_current;
}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

const std::string& current_test() { return g_current; }

void record_assertion() { ++g_assertions; }

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::cout << "FAIL " << g_current << " " << file << ":" << line << ": " << message << "\n";
  std::cout.flush();
}

int run_all(const std::vector<std::string>& filters) {
  std::size_t executed = 0;
  for (const auto& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filters.empty()) {
      bool matched = false;
      for (const auto& filter : filters) {
        if (full.find(filter) != std::string::npos) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        continue;
      }
    }
    g_current = full;
    std::cout << "RUN  " << full << "\n";
    std::cout.flush();
    try {
      test.fn();
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__,
                     std::string("unhandled exception: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    ++executed;
  }
  std::cout << "\n" << executed << " test(s), " << g_assertions << " assertion(s), " << g_failures
            << " failure(s)\n";
  if (executed == 0) {
    std::cout << "no tests matched the given filters\n";
    return 2;
  }
  return g_failures == 0 ? 0 : 1;
}

}  // namespace ifabric_test

namespace {

// A test that throws is a defect in the code under test, not a reason to abort
// the whole run: the exception is reported against the running test and the
// suite continues.
void on_terminate() {
  const std::exception_ptr error = std::current_exception();
  if (error != nullptr) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& caught) {
      std::cout << "FAIL " << ifabric_test::current_test()
                << " unhandled exception: " << caught.what() << "\n";
    } catch (...) {
      std::cout << "FAIL " << ifabric_test::current_test() << " unhandled unknown exception\n";
    }
  } else {
    std::cout << "FAIL " << ifabric_test::current_test()
              << " terminated without an active exception\n";
  }
  std::cout.flush();
  std::abort();
}

#if defined(_WIN32)
// Reports the Windows exception code and, for a fail-fast, the fast-fail
// subcode, together with the test that was running. Diagnostics only: it never
// changes the outcome of a run.
LONG unhandled_exception_filter(EXCEPTION_POINTERS* info) {
  const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
                         ? info->ExceptionRecord->ExceptionCode
                         : 0;
  unsigned long long detail = 0;
  if (info != nullptr && info->ExceptionRecord != nullptr &&
      info->ExceptionRecord->NumberParameters > 0) {
    detail = static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]);
  }
  std::fprintf(stderr, "FATAL %s: exception 0x%08lx detail 0x%llx\n",
               ifabric_test::current_test().c_str(), static_cast<unsigned long>(code), detail);
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  ::SetUnhandledExceptionFilter(&unhandled_exception_filter);
#endif
  std::set_terminate(&on_terminate);
  std::vector<std::string> filters;
  for (int i = 1; i < argc; ++i) {
    filters.emplace_back(argv[i]);
  }
  return ifabric_test::run_all(filters);
}
