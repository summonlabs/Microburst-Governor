// Microburst Governor - synthetic benchmark entry point (SYNTHETIC).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdlib>
#include <string>

#include "mbg/bench/benchmark.hpp"

int main(int argc, char** argv) {
  mbg::bench::BenchmarkConfig config;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const auto value = static_cast<std::uint64_t>(std::strtoull(argv[i + 1], nullptr, 10));
    if (flag == "--resources") {
      config.resources = value;
    } else if (flag == "--ticks") {
      config.ticks = value;
    } else if (flag == "--window") {
      config.window = value;
    } else if (flag == "--bursts") {
      config.bursts_per_resource = value;
    } else if (flag == "--burst-ticks") {
      config.burst_ticks = value;
    } else if (flag == "--interval") {
      config.sample_interval = value;
    } else if (flag == "--seed") {
      config.seed = value;
    } else {
      std::fprintf(stderr, "mbg_bench: unknown option %s\n", flag.c_str());
      return 2;
    }
  }
  const mbg::bench::BenchmarkResult result = mbg::bench::run(config);
  std::fputs(mbg::bench::render(config, result).c_str(), stdout);
  return result.samples_ingested == 0 ? 1 : 0;
}
