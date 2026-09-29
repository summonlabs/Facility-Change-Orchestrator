#pragma once

// Test harness for the Facility Change Orchestrator test executables.
//
// Header-only, C++20, standard library plus "fco/error.hpp" only. The harness
// deliberately avoids any third-party dependency so that the test binaries link
// nothing but the library sources they exercise.
//
// Contract:
//   * One report line per case: "PASS suite.name" or "FAIL suite.name: <text>".
//   * A final summary line: "N passed, M failed, K total".
//   * Exit code 0 when every selected case passed, 1 otherwise.
//   * A failing check never aborts the process and never stops the suite: it
//     marks the current case failed and lets the remaining checks run.
//   * An exception escaping a case body is reported as a failed case.
//   * Optional command-line filter: run_all(argc, argv) runs only the cases
//     whose "suite.name" contains argv[1].
//   * Nothing else is ever written to stdout.

#include <cstddef>
#include <exception>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "fco/error.hpp"

namespace fco::test {

// ---------------------------------------------------------------------------
// Value rendering used by failure messages.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string escape_and_truncate(std::string_view text,
                                                     std::size_t limit = 120) {
  std::string out;
  const std::size_t take = text.size() < limit ? text.size() : limit;
  out.reserve(take + 8);
  for (std::size_t i = 0; i < take; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      out += '.';
    } else {
      out += static_cast<char>(c);
    }
  }
  if (text.size() > take) out += "...";
  return out;
}

template <class T>
concept StreamInsertable = requires(std::ostream& stream, const T& value) { stream << value; };

template <class T>
[[nodiscard]] std::string print_value(const T& value) {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  if constexpr (std::is_same_v<U, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<U>) {
    using Underlying = std::underlying_type_t<U>;
    if constexpr (std::is_signed_v<Underlying>) {
      return std::to_string(static_cast<long long>(value));
    } else {
      return std::to_string(static_cast<unsigned long long>(value));
    }
  } else if constexpr (std::is_integral_v<U>) {
    if constexpr (std::is_signed_v<U>) {
      return std::to_string(static_cast<long long>(value));
    } else {
      return std::to_string(static_cast<unsigned long long>(value));
    }
  } else if constexpr (std::is_floating_point_v<U>) {
    return std::to_string(static_cast<double>(value));
  } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
    return std::string("\"") + escape_and_truncate(std::string_view(value)) + "\"";
  } else if constexpr (StreamInsertable<U>) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<unprintable>";
  }
}

// Customisation point: specialise ValuePrinter<T> for types that deserve a
// richer rendering than the default (for example fco::Digest as hex).
template <class T>
struct ValuePrinter {
  [[nodiscard]] static std::string print(const T& value) { return print_value(value); }
};

template <class T>
[[nodiscard]] std::string debug_string(const T& value) {
  using U = std::remove_cv_t<std::remove_reference_t<T>>;
  return ValuePrinter<U>::print(value);
}

// Sign-safe equality so that a signed/unsigned pairing in a check never turns
// into a compiler warning under /W4 /WX.
template <class T, class U>
[[nodiscard]] constexpr bool equal_values(const T& a, const U& b) {
  if constexpr (std::is_integral_v<T> && std::is_integral_v<U> && !std::is_same_v<T, U> &&
                (std::is_signed_v<T> != std::is_signed_v<U>)) {
    if constexpr (std::is_signed_v<T>) {
      if (a < T{0}) return false;
    }
    if constexpr (std::is_signed_v<U>) {
      if (b < U{0}) return false;
    }
    return static_cast<unsigned long long>(a) == static_cast<unsigned long long>(b);
  } else {
    return a == b;
  }
}

// ---------------------------------------------------------------------------
// Case registry and per-case state.
// ---------------------------------------------------------------------------
struct Case {
  std::string suite;
  std::string name;
  void (*body)();
};

[[nodiscard]] inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

[[nodiscard]] inline bool& case_failed_flag() {
  static bool failed = false;
  return failed;
}

[[nodiscard]] inline std::size_t& case_failure_counter() {
  static std::size_t count = 0;
  return count;
}

[[nodiscard]] inline std::string& case_first_failure() {
  static std::string message;
  return message;
}

inline void begin_case() {
  case_failed_flag() = false;
  case_failure_counter() = 0;
  case_first_failure().clear();
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*body)()) {
    registry().push_back(Case{suite, name, body});
  }
};

// ---------------------------------------------------------------------------
// Failure reporting.
// ---------------------------------------------------------------------------
inline void fail_case(const std::string& message) {
  if (!case_failed_flag()) {
    case_failed_flag() = true;
    case_first_failure() = message;
  }
  ++case_failure_counter();
}

inline void report_failure(const char* file, int line, const std::string& message) {
  std::string text = message;
  if (file != nullptr && line > 0) {
    text += " (at ";
    text += file;
    text += ':';
    text += std::to_string(line);
    text += ')';
  }
  fail_case(text);
}

// ---------------------------------------------------------------------------
// Checks. None of them throws and none of them aborts the process.
// ---------------------------------------------------------------------------
inline void check_true(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    report_failure(file, line, std::string("expected true: ") + expression);
  }
}

template <class T, class U>
inline void check_equal(const T& a, const U& b, const char* ea, const char* eb, const char* file,
                        int line) {
  if (equal_values(a, b)) return;
  std::string message = "expected ";
  message += ea;
  message += " == ";
  message += eb;
  message += ", got ";
  message += debug_string(a);
  message += " vs ";
  message += debug_string(b);
  report_failure(file, line, message);
}

inline void check_error(const fco::Error& error, fco::ErrorCode expected, const char* expression,
                        const char* file, int line) {
  if (error.code == expected) return;
  std::string message = "expected ";
  message += expression;
  message += " to fail with ";
  message += fco::to_string(expected);
  message += ", got ";
  message += fco::describe(error);
  report_failure(file, line, message);
}

// ---------------------------------------------------------------------------
// Runner.
// ---------------------------------------------------------------------------
[[nodiscard]] inline int run_all(int argc, char** argv) {
  std::string filter;
  if (argc > 1 && argv[1] != nullptr) filter = argv[1];

  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const Case& entry : registry()) {
    const std::string full_name = entry.suite + "." + entry.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos) continue;

    begin_case();
    try {
      entry.body();
    } catch (const std::exception& ex) {
      report_failure("<exception>", 0, std::string("uncaught std::exception: ") + ex.what());
    } catch (...) {
      report_failure("<exception>", 0, "uncaught non-standard exception");
    }

    if (case_failed_flag()) {
      ++failed;
      std::string message = case_first_failure();
      if (case_failure_counter() > 1) {
        message += " (+";
        message += std::to_string(case_failure_counter() - 1);
        message += " further failed check(s))";
      }
      std::cout << "FAIL " << full_name << ": " << message << '\n';
    } else {
      ++passed;
      std::cout << "PASS " << full_name << '\n';
    }
  }

  std::cout << passed << " passed, " << failed << " failed, " << (passed + failed) << " total"
            << std::endl;
  return failed == 0 ? 0 : 1;
}

}  // namespace fco::test

// ---------------------------------------------------------------------------
// Macros.
// ---------------------------------------------------------------------------
#define FCO_TEST(suite_name, case_name)                                                 \
  static void fco_test_body_##suite_name##_##case_name();                               \
  static const ::fco::test::Registrar fco_test_registrar_##suite_name##_##case_name(    \
      #suite_name, #case_name, &fco_test_body_##suite_name##_##case_name);              \
  static void fco_test_body_##suite_name##_##case_name()

#define FCO_CHECK(expr) ::fco::test::check_true(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define FCO_REQUIRE(expr)                                                    \
  do {                                                                       \
    if (!static_cast<bool>(expr)) {                                          \
      ::fco::test::report_failure(__FILE__, __LINE__,                        \
                                  std::string("required: ") + #expr);        \
      return;                                                                \
    }                                                                        \
  } while (false)

#define FCO_CHECK_EQ(a, b) ::fco::test::check_equal((a), (b), #a, #b, __FILE__, __LINE__)

#define FCO_CHECK_ERROR(result_expr, expected_code)                                       \
  do {                                                                                    \
    auto&& fco_result_ = (result_expr);                                                   \
    if (fco_result_.ok()) {                                                               \
      ::fco::test::report_failure(                                                        \
          __FILE__, __LINE__,                                                             \
          std::string(#result_expr) + " unexpectedly succeeded; expected " +              \
              std::string(::fco::to_string(expected_code)));                              \
    } else {                                                                              \
      ::fco::test::check_error(fco_result_.error(), (expected_code), #result_expr,        \
                               __FILE__, __LINE__);                                       \
    }                                                                                     \
  } while (false)

#define FCO_TEST_MAIN                                      \
  int main(int argc, char** argv) {                        \
    return ::fco::test::run_all(argc, argv);               \
  }
