// Microburst Governor - synthetic detection benchmark (SYNTHETIC).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/bench/benchmark.hpp"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"

namespace mbg::bench {
namespace {

[[nodiscard]] std::uint64_t now_nanos() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] Policy benchmark_policy(std::uint64_t window) noexcept {
  Policy policy = make_default_policy();
  policy.id = make_policy_id("mbg.benchmark.v1");
  policy.detection.window_capacity = window;
  policy.detection.min_samples_for_classification = window < 8 ? 4 : 8;
  if (policy.detection.min_samples_for_classification > window) {
    policy.detection.min_samples_for_classification = window;
  }
  return policy;
}

}  // namespace

BenchmarkResult run(const BenchmarkConfig& config) {
  BenchmarkResult result;
  result.window_capacity = config.window;

  std::vector<Sample> samples;
  const std::uint64_t generation_start = now_nanos();
  for (std::uint64_t resource = 0; resource < config.resources; ++resource) {
    synthetic::GeneratorConfig generator;
    generator.shape = synthetic::TraceShape::kMicroburst;
    generator.seed = config.seed + resource;
    generator.resource = ResourceId::from_raw(resource + 1U);
    generator.queue = QueueId::from_raw(1);
    generator.path = PathId::from_raw(1);
    generator.ticks = config.ticks;
    generator.sample_interval = config.sample_interval;
    generator.burst_start = 64;
    generator.burst_ticks = config.burst_ticks;
    std::vector<Sample> generated = synthetic::generate(generator);
    // Spread the configured number of bursts across the tick range.
    const std::uint64_t stride = config.ticks / (config.bursts_per_resource + 1U);
    for (std::uint64_t burst = 1; burst <= config.bursts_per_resource; ++burst) {
      synthetic::GeneratorConfig extra = generator;
      extra.burst_start = burst * stride;
      extra.seed = config.seed + resource * 131U + burst;
      std::vector<Sample> more = synthetic::generate(extra);
      generated.insert(generated.end(), more.begin(), more.end());
    }
    samples.insert(samples.end(), generated.begin(), generated.end());
  }
  const std::uint64_t generation_end = now_nanos();
  result.generation_nanos = generation_end - generation_start;
  result.samples_generated = samples.size();

  GovernorConfig governor_config;
  governor_config.policy = benchmark_policy(config.window);
  governor_config.epoch = Epoch::from_raw(1);
  governor_config.incarnation = IncarnationId::from_raw(0xB0B0B0B0ULL);
  governor_config.boot = BootId::from_raw(1);
  governor_config.enable_interventions = config.enable_interventions;

  const std::uint64_t detection_start = now_nanos();
  {
    Governor governor(governor_config);
    Tick highest{};
    for (const Sample& sample : samples) {
      const IngestOutcome outcome = governor.ingest(sample);
      if (outcome.status.ok()) {
        result.samples_ingested += 1;
      } else {
        result.samples_rejected += 1;
      }
      if (sample.tick.value() > highest.value()) {
        highest = sample.tick;
      }
    }
    static_cast<void>(governor.advance(Tick::from_raw(highest.value() + 4096U)));
    const GovernorStats stats = governor.stats();
    result.decisions = stats.decisions;
    result.episodes_opened = stats.episodes_opened;
    result.episodes_closed = stats.episodes_closed;
    result.interventions_requested = stats.interventions_requested;
    result.streams = governor.stream_count();
  }
  const std::uint64_t detection_end = now_nanos();
  result.detection_nanos = detection_end - detection_start;

  if (result.detection_nanos != 0) {
    const double seconds = static_cast<double>(result.detection_nanos) / 1.0e9;
    result.samples_per_second = static_cast<double>(result.samples_ingested) / seconds;
    result.nanoseconds_per_sample =
        static_cast<double>(result.detection_nanos) / static_cast<double>(result.samples_ingested == 0 ? 1 : result.samples_ingested);
    result.episodes_per_second = static_cast<double>(result.episodes_opened) / seconds;
  }
  return result;
}

std::string render(const BenchmarkConfig& config, const BenchmarkResult& result) {
  std::ostringstream out;
  out << "microburst-governor benchmark [SYNTHETIC]\n";
  out << "  resources=" << config.resources << " ticks=" << config.ticks
      << " sample_interval=" << config.sample_interval << " window=" << config.window
      << " bursts_per_resource=" << config.bursts_per_resource
      << " burst_ticks=" << config.burst_ticks << " seed=" << config.seed << "\n";
  out << "  samples_generated=" << result.samples_generated
      << " samples_ingested=" << result.samples_ingested
      << " samples_rejected=" << result.samples_rejected << "\n";
  out << "  decisions=" << result.decisions << " episodes_opened=" << result.episodes_opened
      << " episodes_closed=" << result.episodes_closed
      << " interventions_requested=" << result.interventions_requested
      << " streams=" << result.streams << "\n";
  out << "  generation_ms=" << (static_cast<double>(result.generation_nanos) / 1.0e6)
      << " detection_ms=" << (static_cast<double>(result.detection_nanos) / 1.0e6) << "\n";
  out << "  completed_samples_per_second=" << result.samples_per_second
      << " ns_per_completed_sample=" << result.nanoseconds_per_sample
      << " episodes_per_second=" << result.episodes_per_second << "\n";
  out << "  NOTE: synthetic population, not a physical network measurement.\n";
  return out.str();
}

}  // namespace mbg::bench
