// Microburst Governor - minimal test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace mbg::test {

/// Thrown by MBG_REQUIRE to abandon a test case whose preconditions failed.
class Abort {};

using Body = void (*)();

struct Case {
  const char* suite;
  const char* name;
  Body body;
};

[[nodiscard]] std::vector<Case>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, Body body);
};

/// Records a failure and continues the current case.
void record_failure(const char* file, int line, const std::string& message);

/// Renders a value for a failure message.
template <class T>
[[nodiscard]] std::string render(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else {
    return std::string("<value>");
  }
}

/// Runs every registered case. Returns 0 only when every check passed.
int run_all(int argc, char** argv);

/// Temporary directory helper: creates a unique directory and removes it on destruction.
class TempDir {
 public:
  explicit TempDir(const char* tag);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_{};
};

/// Polls until the predicate holds, bounded so a broken system fails the test loudly instead of
/// hanging the suite. This is a synchronisation wait, never a watchdog that hides a stall.
[[nodiscard]] bool await(const std::function<bool()>& predicate, std::uint64_t max_polls = 60000,
                         std::uint64_t poll_millis = 1);

}  // namespace mbg::test

#define MBG_TEST(suite, name)                                                        \
  static void suite##_##name##_body();                                               \
  static const ::mbg::test::Registrar suite##_##name##_registrar(#suite, #name,      \
                                                                 &suite##_##name##_body); \
  static void suite##_##name##_body()

#define MBG_CHECK(condition)                                                        \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::mbg::test::record_failure(__FILE__, __LINE__, "check failed: " #condition); \
    }                                                                               \
  } while (false)

#define MBG_CHECK_EQ(actual, expected)                                                    \
  do {                                                                                    \
    const auto mbg_actual = (actual);                                                      \
    const auto mbg_expected = (expected);                                                  \
    if (!(mbg_actual == mbg_expected)) {                                                   \
      ::mbg::test::record_failure(                                                         \
          __FILE__, __LINE__,                                                              \
          std::string("check failed: " #actual " == " #expected " (actual=")               \
              .append(::mbg::test::render(mbg_actual))                                     \
              .append(" expected=")                                                        \
              .append(::mbg::test::render(mbg_expected))                                   \
              .append(")"));                                                               \
    }                                                                                      \
  } while (false)

#define MBG_REQUIRE(condition)                                                           \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      ::mbg::test::record_failure(__FILE__, __LINE__, "requirement failed: " #condition); \
      throw ::mbg::test::Abort{};                                                        \
    }                                                                                    \
  } while (false)
