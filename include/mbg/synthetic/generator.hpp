// Microburst Governor - synthetic evidence generation (SYNTHETIC, never physical).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/model/sample.hpp"

namespace mbg::synthetic {

/// Synthetic traffic shapes. Every sample produced here is labelled with SampleFlag::kSynthetic and
/// is never presented as a physical network measurement.
enum class TraceShape : std::uint8_t {
  kSteady = 0,
  kMicroburst = 1,
  kRamp = 2,
  kOscillating = 3,
  kNoisySteady = 4,
  kSparseSampling = 5,
  kCounterReset = 6,
  kCapacityChange = 7,
  kMissingSamples = 8,
  kReordered = 9,
  kDuplicate = 10,
  kShallowNoise = 11,
  kLongBurst = 12,
};

[[nodiscard]] const char* to_string(TraceShape shape) noexcept;
[[nodiscard]] bool parse_shape(std::string_view text, TraceShape& shape) noexcept;
[[nodiscard]] std::string shape_names();

struct GeneratorConfig {
  TraceShape shape{TraceShape::kMicroburst};
  std::uint64_t seed{1};
  ResourceId resource{ResourceId::from_raw(1)};
  QueueId queue{QueueId::from_raw(1)};
  PathId path{PathId::from_raw(1)};
  ResourceGeneration resource_generation{ResourceGeneration::from_raw(1)};
  ProvenanceId provenance{ProvenanceId::from_raw(1)};

  std::uint64_t start_tick{0};
  std::uint64_t ticks{512};
  std::uint64_t sample_interval{1};
  std::uint64_t burst_start{128};
  std::uint64_t burst_ticks{24};

  std::uint64_t baseline_depth{128};
  std::uint64_t peak_depth{9216};
  std::uint64_t baseline_occupancy_bytes{1U << 14};
  std::uint64_t peak_occupancy_bytes{1U << 19};
  std::uint64_t capacity_bytes{1U << 20};

  std::uint64_t baseline_rate_q16{rate_from_whole(120)};
  std::uint64_t peak_rate_q16{rate_from_whole(9000)};
  std::uint64_t drain_rate_q16{rate_from_whole(4000)};
};

/// Generates a deterministic synthetic trace. Identical configuration plus seed yields identical
/// bytes, which is what makes replay based identity and classification proofs meaningful.
[[nodiscard]] std::vector<Sample> generate(const GeneratorConfig& config);

// --- line based trace format -------------------------------------------------

/// Renders one sample as a single bounded line.
[[nodiscard]] std::string encode_line(const Sample& sample);

/// Parses one line. Rejects malformed, oversized or structurally invalid input with a status.
[[nodiscard]] Status decode_line(std::string_view line, Sample& sample);

/// Renders a whole trace.
[[nodiscard]] std::string encode_trace(const std::vector<Sample>& samples);

/// Parses a whole trace, rejecting the first malformed line.
[[nodiscard]] Status decode_trace(std::string_view text, std::vector<Sample>& samples,
                                  std::size_t max_samples);

}  // namespace mbg::synthetic
