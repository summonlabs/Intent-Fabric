// Intent Fabric - authoritative desired-state intent runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Minimal test harness. Tests run plainly to completion; there are no
// per-test timeouts and no watchdogs. A test that hangs is a defect.

#ifndef IFABRIC_TEST_HPP
#define IFABRIC_TEST_HPP

#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ifabric_test {

struct TestCase {
  std::string suite;
  std::string name;
  void (*fn)();
};

std::vector<TestCase>& registry();
const std::string& current_test();
void record_assertion();
void report_failure(const char* file, int line, const std::string& message);
int run_all(const std::vector<std::string>& filters);

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
  }
};

template <class T>
std::string display(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    std::ostringstream out;
    out << static_cast<long long>(static_cast<std::underlying_type_t<T>>(value));
    return out.str();
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (std::is_convertible_v<T, std::string>) {
    return "\"" + static_cast<std::string>(value) + "\"";
  } else {
    return "<value>";
  }
}

}  // namespace ifabric_test

#define IFABRIC_TEST(suite, name)                                                    \
  static void suite##_##name##_body();                                               \
  static const ::ifabric_test::Registrar ifabric_registrar_##suite##_##name(         \
      #suite, #name, &suite##_##name##_body);                                        \
  static void suite##_##name##_body()

#define CHECK(expr)                                                                  \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    if (!(expr)) {                                                                   \
      ::ifabric_test::report_failure(__FILE__, __LINE__, "CHECK failed: " #expr);    \
    }                                                                                \
  } while (false)

#define REQUIRE(expr)                                                                \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    if (!(expr)) {                                                                   \
      ::ifabric_test::report_failure(__FILE__, __LINE__, "REQUIRE failed: " #expr);  \
      return;                                                                        \
    }                                                                                \
  } while (false)

#define CHECK_EQ(a, b)                                                               \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    const auto ifabric_left = (a);                                                   \
    const auto ifabric_right = (b);                                                  \
    if (!(ifabric_left == ifabric_right)) {                                          \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__,                                                        \
          std::string("CHECK_EQ failed: " #a " == " #b " (") +                       \
              ::ifabric_test::display(ifabric_left) + " vs " +                       \
              ::ifabric_test::display(ifabric_right) + ")");                         \
    }                                                                                \
  } while (false)

#define CHECK_NE(a, b)                                                               \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    const auto ifabric_left = (a);                                                   \
    const auto ifabric_right = (b);                                                  \
    if (ifabric_left == ifabric_right) {                                             \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__, std::string("CHECK_NE failed: " #a " != " #b));        \
    }                                                                                \
  } while (false)

// Asserts that an expression yields an Error (or a failed Result) with a code.
#define CHECK_ERROR_CODE(expr, expected)                                             \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    const auto& ifabric_error = (expr);                                              \
    if (ifabric_error.ok()) {                                                        \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__, std::string("expected an error from " #expr));         \
    } else if (ifabric_error.code != (expected)) {                                   \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__,                                                        \
          std::string("unexpected error code from " #expr ": ") +                    \
              std::string(::ifabric::to_string(ifabric_error.code)) + " != " +       \
              std::string(::ifabric::to_string(expected)));                          \
    }                                                                                \
  } while (false)

#define CHECK_OK(expr)                                                               \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    const auto& ifabric_result = (expr);                                             \
    if (!ifabric_result) {                                                           \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__,                                                        \
          std::string("expected success from " #expr ": ") +                         \
              ifabric_result.error().render());                                      \
    }                                                                                \
  } while (false)

#define CHECK_RESULT_OK(expr)                                                        \
  do {                                                                               \
    ::ifabric_test::record_assertion();                                              \
    const auto& ifabric_result = (expr);                                             \
    if (!ifabric_result) {                                                           \
      ::ifabric_test::report_failure(                                                \
          __FILE__, __LINE__,                                                        \
          std::string("expected a value from " #expr ": ") +                         \
              ifabric_result.error().render());                                      \
    }                                                                                \
  } while (false)

#endif  // IFABRIC_TEST_HPP
