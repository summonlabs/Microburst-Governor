// Microburst Governor - mbgctl generate.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "cli_common.hpp"
#include "mbg/synthetic/generator.hpp"

namespace mbg::cli {

int run_generate(const Arguments& arguments) {
  synthetic::GeneratorConfig config;
  const std::optional<std::string> shape = arguments.value("--shape");
  if (shape.has_value() && !synthetic::parse_shape(*shape, config.shape)) {
    std::fprintf(stderr, "mbgctl generate: unknown shape '%s'\n", shape->c_str());
    return 2;
  }
  if (const auto value = arguments.number("--ticks"); value.has_value()) {
    config.ticks = *value;
  }
  if (const auto value = arguments.number("--seed"); value.has_value()) {
    config.seed = *value;
  }
  if (const auto value = arguments.number("--interval"); value.has_value()) {
    config.sample_interval = *value;
  }
  if (const auto value = arguments.number("--burst-start"); value.has_value()) {
    config.burst_start = *value;
  }
  if (const auto value = arguments.number("--burst-ticks"); value.has_value()) {
    config.burst_ticks = *value;
  }
  const std::vector<Sample> samples = synthetic::generate(config);
  const std::string trace = synthetic::encode_trace(samples);
  if (const std::optional<std::string> out = arguments.value("--out"); out.has_value()) {
    if (!write_file(*out, trace)) {
      std::fputs("mbgctl generate: output file could not be written\n", stderr);
      return 2;
    }
    std::printf("generate: shape=%s samples=%zu bytes=%zu file=%s\n",
                synthetic::to_string(config.shape), samples.size(), trace.size(), out->c_str());
    return 0;
  }
  std::fputs(trace.c_str(), stdout);
  return 0;
}

}  // namespace mbg::cli
