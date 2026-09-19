// Microburst Governor - minimal test framework.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "mbg/platform/process.hpp"

namespace mbg::test {
namespace {

std::atomic<std::uint64_t> g_case_failures{0};
std::atomic<std::uint64_t> g_total_failures{0};
std::atomic<std::uint64_t> g_cases_run{0};
std::string g_current_case;
std::atomic<std::uint64_t> g_temp_counter{0};

}  // namespace

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
  registry().push_back(Case{suite, name, body});
}

void record_failure(const char* file, int line, const std::string& message) {
  g_case_failures.fetch_add(1);
  g_total_failures.fetch_add(1);
  std::fprintf(stderr, "FAIL %s: %s:%d: %s\n", g_current_case.c_str(), file, line, message.c_str());
}

TempDir::TempDir(const char* tag) {
  const std::uint64_t counter = g_temp_counter.fetch_add(1) + 1U;
  std::filesystem::path base = std::filesystem::temp_directory_path();
  std::string name = "mbg-test-";
  name.append(tag);
  name.push_back('-');
  name.append(std::to_string(platform::process_id()));
  name.push_back('-');
  name.append(std::to_string(counter));
  path_ = (base / name).string();
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
  std::filesystem::create_directories(path_, ec);
}

TempDir::~TempDir() {
  std::error_code ec;
  std::filesystem::remove_all(path_, ec);
}

bool await(const std::function<bool()>& predicate, std::uint64_t max_polls,
           std::uint64_t poll_millis) {
  for (std::uint64_t i = 0; i < max_polls; ++i) {
    if (predicate()) {
      return true;
    }
    platform::sleep_millis(poll_millis);
  }
  return predicate();
}

int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  std::vector<Case> cases = registry();
  std::uint64_t ran = 0;
  for (const Case& test_case : cases) {
    if (filter != nullptr) {
      const std::string full = std::string(test_case.suite) + "." + test_case.name;
      if (full.find(filter) == std::string::npos) {
        continue;
      }
    }
    g_current_case = std::string(test_case.suite) + "." + test_case.name;
    g_case_failures.store(0);
    std::printf("RUN  %s\n", g_current_case.c_str());
    std::fflush(stdout);
    try {
      test_case.body();
    } catch (const Abort&) {
      // the case already recorded the requirement failure
    } catch (const std::exception& error) {
      record_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
    } catch (...) {
      record_failure(__FILE__, __LINE__, "unexpected non standard exception");
    }
    ran += 1;
    g_cases_run.fetch_add(1);
    if (g_case_failures.load() == 0) {
      std::printf("PASS %s\n", g_current_case.c_str());
    } else {
      std::printf("FAIL %s (%llu check%s)\n", g_current_case.c_str(),
                  static_cast<unsigned long long>(g_case_failures.load()),
                  g_case_failures.load() == 1 ? "" : "s");
    }
    std::fflush(stdout);
  }
  std::printf("---- %llu case(s) run, %llu failure(s)\n", static_cast<unsigned long long>(ran),
              static_cast<unsigned long long>(g_total_failures.load()));
  if (ran == 0) {
    std::fputs("no test case matched the filter\n", stderr);
    return 2;
  }
  return g_total_failures.load() == 0 ? 0 : 1;
}

}  // namespace mbg::test
