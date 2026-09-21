// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test harness.
//
// Deliberately free of any timeout, watchdog or signal machinery: a hanging
// test is a defect in the code under test and must be diagnosed rather than
// hidden. Tests run to completion naturally.

#ifndef SUMMON_FABRIC_RECONCILIATION_TEST_SUPPORT_HPP
#define SUMMON_FABRIC_RECONCILIATION_TEST_SUPPORT_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace frtest {

struct TestCase {
  const char* suite;
  const char* name;
  std::function<void()> body;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry().push_back(TestCase{suite, name, std::move(body)});
  }
};

struct Failure {
  std::string message;
};

[[noreturn]] inline void Fail(const char* file, int line, const std::string& message) {
  std::string text = std::string(file) + ":" + std::to_string(line) + ": " + message;
  throw Failure{text};
}

inline void CheckTrue(const char* file, int line, bool condition, const char* expression) {
  if (!condition) {
    Fail(file, line, std::string("expected true: ") + expression);
  }
}

template <typename A, typename B>
void CheckEqual(const char* file, int line, const A& actual, const B& expected,
                const char* expression) {
  if (!(actual == expected)) {
    Fail(file, line, std::string("expected equality: ") + expression);
  }
}

inline int RunAll(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : Registry()) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (filter != nullptr && full.find(filter) == std::string::npos) {
      continue;
    }
    std::printf("RUN  %s\n", full.c_str());
    std::fflush(stdout);
    try {
      test.body();
      ++passed;
      std::printf("PASS %s\n", full.c_str());
      std::fflush(stdout);
    } catch (const Failure& failure) {
      ++failed;
      std::printf("FAIL %s\n  %s\n", full.c_str(), failure.message.c_str());
      std::fflush(stdout);
    } catch (const std::exception& error) {
      ++failed;
      std::printf("FAIL %s\n  unexpected exception: %s\n", full.c_str(), error.what());
      std::fflush(stdout);
    } catch (...) {
      ++failed;
      std::printf("FAIL %s\n  unexpected non-standard exception\n", full.c_str());
      std::fflush(stdout);
    }
  }
  std::printf("SUMMARY passed=%zu failed=%zu\n", passed, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace frtest

#define FR_TEST(suite, name)                                                      \
  static void fr_test_##suite##_##name();                                         \
  static const ::frtest::Registrar fr_registrar_##suite##_##name(                 \
      #suite, #name, &fr_test_##suite##_##name);                                  \
  static void fr_test_##suite##_##name()

#define FR_CHECK(expression) ::frtest::CheckTrue(__FILE__, __LINE__, (expression), #expression)
#define FR_CHECK_EQ(actual, expected) \
  ::frtest::CheckEqual(__FILE__, __LINE__, (actual), (expected), #actual " == " #expected)
#define FR_REQUIRE(expression)                       \
  do {                                               \
    if (!(expression)) {                             \
      ::frtest::Fail(__FILE__, __LINE__,             \
                     std::string("require failed: ") + #expression); \
    }                                                \
  } while (false)

#define FR_CHECK_STATUS_OK(result)                                                        \
  do {                                                                                    \
    const auto fr_status_check = (result);                                                \
    if (!fr_status_check.ok()) {                                                          \
      ::frtest::Fail(__FILE__, __LINE__,                                                  \
                     std::string("status not ok: ") + fr_status_check.ToString());        \
    }                                                                                     \
  } while (false)

#endif  // SUMMON_FABRIC_RECONCILIATION_TEST_SUPPORT_HPP
