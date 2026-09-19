// Microburst Governor - synthetic detection benchmark (SYNTHETIC).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace mbg::bench {

/// Benchmark parameters. The population is generated synthetically: nothing here measures a physical
/// network, a NIC, a switch or a real queue.
struct BenchmarkConfig {
  std::uint64_t resources{8};
  std::uint64_t ticks{4096};
  std::uint64_t sample_interval{1};
  std::uint64_t window{64};
  std::uint64_t bursts_per_resource{4};
  std::uint64_t burst_ticks{16};
  std::uint64_t seed{20260101};
  bool enable_interventions{true};
};

struct BenchmarkResult {
  std::uint64_t samples_generated{0};
  std::uint64_t samples_ingested{0};
  std::uint64_t samples_rejected{0};
  std::uint64_t decisions{0};
  std::uint64_t episodes_opened{0};
  std::uint64_t episodes_closed{0};
  std::uint64_t interventions_requested{0};
  std::uint64_t streams{0};
  std::uint64_t window_capacity{0};

  std::uint64_t generation_nanos{0};
  std::uint64_t detection_nanos{0};

  /// Rates are derived from completed work: every counted sample was fully ingested and evaluated
  /// before the clock stopped. Submission or enqueue latency is never reported as throughput.
  double samples_per_second{0};
  double nanoseconds_per_sample{0};
  double episodes_per_second{0};
};

[[nodiscard]] BenchmarkResult run(const BenchmarkConfig& config);
[[nodiscard]] std::string render(const BenchmarkConfig& config, const BenchmarkResult& result);

}  // namespace mbg::bench
