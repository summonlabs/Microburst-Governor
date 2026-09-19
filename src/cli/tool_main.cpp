// Microburst Governor - mbgctl entry point.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>
#include <vector>

#include "cli_common.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> tokens;
  tokens.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    tokens.emplace_back(argv[i]);
  }
  if (tokens.empty()) {
    mbg::cli::print_usage();
    return 2;
  }
  const std::string command = tokens.front();
  const mbg::cli::Arguments arguments(std::vector<std::string>(tokens.begin() + 1, tokens.end()));

  if (command == "selftest") {
    return mbg::cli::run_selftest(arguments);
  }
  if (command == "generate") {
    return mbg::cli::run_generate(arguments);
  }
  if (command == "replay") {
    return mbg::cli::run_replay(arguments);
  }
  if (command == "verify") {
    return mbg::cli::run_verify(arguments);
  }
  if (command == "bench") {
    return mbg::cli::run_bench(arguments);
  }
  if (command == "--help" || command == "-h" || command == "help") {
    mbg::cli::print_usage();
    return 0;
  }
  std::fprintf(stderr, "mbgctl: unknown command '%s'\n", command.c_str());
  mbg::cli::print_usage();
  return 2;
}
