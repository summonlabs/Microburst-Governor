// Microburst Governor - mbgctl bench.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "cli_common.hpp"
#include "mbg/bench/benchmark.hpp"

namespace mbg::cli {

int run_bench(const Arguments& arguments) {
  bench::BenchmarkConfig config;
  if (const auto value = arguments.number("--resources"); value.has_value()) {
    config.resources = *value;
  }
  if (const auto value = arguments.number("--ticks"); value.has_value()) {
    config.ticks = *value;
  }
  if (const auto value = arguments.number("--sample-interval"); value.has_value()) {
    config.sample_interval = *value;
  }
  if (const auto value = arguments.number("--window"); value.has_value()) {
    config.window = *value;
  }
  if (const auto value = arguments.number("--bursts"); value.has_value()) {
    config.bursts_per_resource = *value;
  }
  if (const auto value = arguments.number("--burst-ticks"); value.has_value()) {
    config.burst_ticks = *value;
  }
  if (const auto value = arguments.number("--seed"); value.has_value()) {
    config.seed = *value;
  }
  const bench::BenchmarkResult result = bench::run(config);
  std::fputs(bench::render(config, result).c_str(), stdout);
  return 0;
}

}  // namespace mbg::cli
