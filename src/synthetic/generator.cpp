// Microburst Governor - synthetic evidence generation (SYNTHETIC, never physical).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/synthetic/generator.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>

#include "mbg/core/checked.hpp"
#include "mbg/core/hash.hpp"
#include "mbg/limits.hpp"

namespace mbg::synthetic {
namespace {

[[nodiscard]] std::uint64_t jitter(DeterministicRng& rng, std::uint64_t amplitude) noexcept {
  if (amplitude == 0) {
    return 0;
  }
  return rng.bounded(amplitude * 2U + 1U) - std::min(amplitude, rng.bounded(amplitude + 1U));
}

[[nodiscard]] bool parse_u64(std::string_view token, std::uint64_t& out) noexcept {
  if (token.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const auto* first = token.data();
  const auto* last = token.data() + token.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

const char* to_string(TraceShape shape) noexcept {
  switch (shape) {
    case TraceShape::kSteady: return "steady";
    case TraceShape::kMicroburst: return "microburst";
    case TraceShape::kRamp: return "ramp";
    case TraceShape::kOscillating: return "oscillating";
    case TraceShape::kNoisySteady: return "noisy-steady";
    case TraceShape::kSparseSampling: return "sparse-sampling";
    case TraceShape::kCounterReset: return "counter-reset";
    case TraceShape::kCapacityChange: return "capacity-change";
    case TraceShape::kMissingSamples: return "missing-samples";
    case TraceShape::kReordered: return "reordered";
    case TraceShape::kDuplicate: return "duplicate";
    case TraceShape::kShallowNoise: return "shallow-noise";
    case TraceShape::kLongBurst: return "long-burst";
  }
  return "unknown";
}

bool parse_shape(std::string_view text, TraceShape& shape) noexcept {
  static constexpr std::array<TraceShape, 13> kShapes = {
      TraceShape::kSteady,        TraceShape::kMicroburst,     TraceShape::kRamp,
      TraceShape::kOscillating,   TraceShape::kNoisySteady,    TraceShape::kSparseSampling,
      TraceShape::kCounterReset,  TraceShape::kCapacityChange, TraceShape::kMissingSamples,
      TraceShape::kReordered,     TraceShape::kDuplicate,      TraceShape::kShallowNoise,
      TraceShape::kLongBurst};
  for (const TraceShape candidate : kShapes) {
    if (text == to_string(candidate)) {
      shape = candidate;
      return true;
    }
  }
  return false;
}

std::string shape_names() {
  std::string out;
  static constexpr std::array<TraceShape, 13> kShapes = {
      TraceShape::kSteady,        TraceShape::kMicroburst,     TraceShape::kRamp,
      TraceShape::kOscillating,   TraceShape::kNoisySteady,    TraceShape::kSparseSampling,
      TraceShape::kCounterReset,  TraceShape::kCapacityChange, TraceShape::kMissingSamples,
      TraceShape::kReordered,     TraceShape::kDuplicate,      TraceShape::kShallowNoise,
      TraceShape::kLongBurst};
  for (const TraceShape candidate : kShapes) {
    if (!out.empty()) {
      out.append(", ");
    }
    out.append(to_string(candidate));
  }
  return out;
}

std::vector<Sample> generate(const GeneratorConfig& config) {
  std::vector<Sample> samples;
  DeterministicRng rng(config.seed);
  const std::uint64_t interval = std::max<std::uint64_t>(config.sample_interval, 1U);
  const std::uint64_t tick_count = std::min<std::uint64_t>(config.ticks, 1U << 20);
  samples.reserve(static_cast<std::size_t>(tick_count / interval + 2U));

  std::uint64_t ingress_total = 0;
  std::uint64_t egress_total = 0;
  std::uint64_t drop_total = 0;
  std::uint64_t mark_total = 0;
  std::uint64_t sequence = 1;
  std::uint64_t capacity = config.capacity_bytes;

  const auto burst_begin = config.burst_start;
  const auto burst_end = config.burst_start + config.burst_ticks;

  for (std::uint64_t index = 0; index <= tick_count; index += interval) {
    const std::uint64_t tick = config.start_tick + index;
    const bool in_burst = index >= burst_begin && index < burst_end;

    Sample sample;
    sample.stream.resource = config.resource;
    sample.stream.queue = config.queue;
    sample.stream.path = config.path;
    sample.resource_generation = config.resource_generation;
    sample.provenance = config.provenance;
    sample.tick = Tick::from_raw(tick);
    sample.seq = SampleSeq::from_raw(sequence);
    sample.flags = SampleFlag::kSynthetic;
    sample.capacity_bytes = capacity;

    std::uint64_t depth = config.baseline_depth;
    std::uint64_t occupancy = config.baseline_occupancy_bytes;
    std::uint64_t ingress = config.baseline_rate_q16;
    std::uint64_t egress = config.baseline_rate_q16;

    switch (config.shape) {
      case TraceShape::kSteady:
        break;
      case TraceShape::kMicroburst: {
        if (in_burst) {
          depth = config.peak_depth;
          occupancy = config.peak_occupancy_bytes;
          ingress = config.peak_rate_q16;
          egress = config.drain_rate_q16;
        }
        break;
      }
      case TraceShape::kLongBurst: {
        if (index >= burst_begin && index < burst_begin + config.burst_ticks * 4U) {
          depth = config.baseline_depth + (config.peak_depth - config.baseline_depth) / 2U;
          occupancy = config.baseline_occupancy_bytes + 4096U;
          ingress = config.peak_rate_q16 / 2U;
          egress = config.drain_rate_q16;
        }
        break;
      }
      case TraceShape::kRamp: {
        if (index >= burst_begin && index < burst_end) {
          const std::uint64_t span = std::max<std::uint64_t>(config.burst_ticks, 1U);
          const std::uint64_t progress = index - burst_begin;
          const std::uint64_t steps = config.peak_depth - config.baseline_depth;
          depth = config.baseline_depth + (steps * progress) / span;
          occupancy = config.baseline_occupancy_bytes + 4096U * progress;
          ingress = config.peak_rate_q16;
          egress = config.baseline_rate_q16;
        }
        break;
      }
      case TraceShape::kOscillating: {
        const std::uint64_t period = std::max<std::uint64_t>(config.burst_ticks, 2U);
        const std::uint64_t phase = index % period;
        if (phase < period / 2U) {
          depth = config.peak_depth;
          occupancy = config.peak_occupancy_bytes;
          ingress = config.peak_rate_q16;
          egress = config.drain_rate_q16;
        }
        break;
      }
      case TraceShape::kNoisySteady:
      case TraceShape::kShallowNoise: {
        const std::uint64_t amplitude =
            config.shape == TraceShape::kShallowNoise ? config.baseline_depth / 4U
                                                      : config.baseline_depth / 2U;
        const std::uint64_t noise = rng.bounded(amplitude + 1U);
        depth = config.baseline_depth + noise;
        occupancy = config.baseline_occupancy_bytes + noise * 16U;
        ingress = config.baseline_rate_q16 + static_cast<RateQ16>(rng.bounded(1024));
        egress = config.baseline_rate_q16;
        break;
      }
      case TraceShape::kSparseSampling:
      case TraceShape::kMissingSamples: {
        sample.flags = sample.flags | SampleFlag::kSparse;
        if (config.shape == TraceShape::kSparseSampling) {
          if ((index / interval) % 5U != 0U) {
            continue;
          }
        } else if (in_burst) {
          continue;  // the burst window loses its samples entirely
        }
        if (in_burst) {
          depth = config.peak_depth;
          occupancy = config.peak_occupancy_bytes;
          ingress = config.peak_rate_q16;
        }
        break;
      }
      case TraceShape::kCounterReset: {
        if (index == burst_begin) {
          ingress_total = 0;
          egress_total = 0;
          drop_total = 0;
          mark_total = 0;
          sample.flags = sample.flags | SampleFlag::kCounterReset;
        }
        if (in_burst) {
          depth = config.peak_depth;
          occupancy = config.peak_occupancy_bytes;
          ingress = config.peak_rate_q16;
        }
        break;
      }
      case TraceShape::kCapacityChange: {
        if (in_burst) {
          capacity = config.capacity_bytes / 2U;
          sample.flags = sample.flags | SampleFlag::kCapacityChange;
          occupancy = std::min(config.peak_occupancy_bytes, capacity);
          depth = config.peak_depth;
          ingress = config.peak_rate_q16;
        }
        sample.capacity_bytes = capacity;
        break;
      }
      case TraceShape::kReordered:
      case TraceShape::kDuplicate: {
        if (in_burst) {
          depth = config.peak_depth;
          occupancy = config.peak_occupancy_bytes;
          ingress = config.peak_rate_q16;
        }
        break;
      }
    }

    ingress_total += static_cast<std::uint64_t>(std::max<RateQ16>(ingress, 0) >> kRateFractionBits);
    egress_total += static_cast<std::uint64_t>(std::max<RateQ16>(egress, 0) >> kRateFractionBits);
    if (in_burst && config.shape != TraceShape::kSteady) {
      drop_total += 3;
      mark_total += 7;
    }
    sample.depth = depth;
    sample.occupancy_bytes = occupancy;
    sample.ingress_rate_q16 = static_cast<RateQ16>(ingress);
    sample.egress_rate_q16 = static_cast<RateQ16>(egress);
    sample.ingress_total = ingress_total;
    sample.egress_total = egress_total;
    sample.drop_total = drop_total;
    sample.mark_total = mark_total;
    samples.push_back(sample);
    sequence += 1;
  }

  if (config.shape == TraceShape::kReordered) {
    for (std::size_t i = 1; i + 1 < samples.size(); i += 7) {
      std::swap(samples[i], samples[i + 1]);
    }
  } else if (config.shape == TraceShape::kDuplicate) {
    std::vector<Sample> duplicated;
    duplicated.reserve(samples.size() * 2U);
    for (const Sample& sample : samples) {
      duplicated.push_back(sample);
      if ((sample.seq.raw() % 11U) == 0U) {
        duplicated.push_back(sample);
      }
    }
    samples.swap(duplicated);
  }
  return samples;
}

std::string encode_line(const Sample& sample) {
  std::string out;
  out.reserve(160);
  out.append(std::to_string(sample.tick.value()));
  out.push_back(' ');
  out.append(std::to_string(sample.stream.resource.raw()));
  out.push_back(' ');
  out.append(std::to_string(sample.stream.queue.raw()));
  out.push_back(' ');
  out.append(std::to_string(sample.depth));
  out.push_back(' ');
  out.append(std::to_string(sample.occupancy_bytes));
  out.push_back(' ');
  out.append(std::to_string(sample.capacity_bytes));
  out.push_back(' ');
  out.append(std::to_string(sample.ingress_rate_q16));
  out.push_back(' ');
  out.append(std::to_string(sample.egress_rate_q16));
  out.push_back(' ');
  out.append(std::to_string(sample.ingress_total));
  out.push_back(' ');
  out.append(std::to_string(sample.egress_total));
  out.push_back(' ');
  out.append(std::to_string(sample.drop_total));
  out.push_back(' ');
  out.append(std::to_string(sample.mark_total));
  out.push_back(' ');
  out.append(std::to_string(static_cast<std::uint32_t>(sample.flags)));
  out.push_back(' ');
  out.append(std::to_string(sample.resource_generation.raw()));
  if (out.size() > limits::kMaxTraceLineBytes) {
    out.resize(limits::kMaxTraceLineBytes);
  }
  return out;
}

Status decode_line(std::string_view line, Sample& sample) {
  if (line.size() > limits::kMaxTraceLineBytes) {
    return Status::failure(StatusCode::kOversized, "trace line exceeds the bound");
  }
  std::istringstream stream{std::string(line)};
  std::string token;
  std::uint64_t values[14] = {};
  std::size_t count = 0;
  while (stream >> token) {
    if (count >= 14) {
      return Status::failure(StatusCode::kInvalidArgument, "trace line has too many fields");
    }
    if (!parse_u64(token, values[count])) {
      return Status::failure(StatusCode::kInvalidArgument, "trace line has a non numeric field");
    }
    count += 1;
  }
  if (count != 14) {
    return Status::failure(StatusCode::kTruncated, "trace line is missing fields");
  }
  // Every field is range checked before it is narrowed: a truncated identity is a corrupted
  // identity, not a repaired one.
  const std::uint64_t narrow_fields[3] = {values[6], values[7], values[13]};
  if (narrow_fields[2] > 0xFFFFFFFFULL) {
    return Status::failure(StatusCode::kOutOfRange, "trace resource generation exceeds 32 bits");
  }
  if (narrow_fields[0] > 0xFFFFFFFFULL || narrow_fields[1] > 0xFFFFFFFFULL) {
    return Status::failure(StatusCode::kOutOfRange, "trace rate exceeds the supported range");
  }
  if (values[12] > 0xFFFFFFFFULL) {
    return Status::failure(StatusCode::kOutOfRange, "trace flags exceed the supported range");
  }
  sample = Sample{};
  sample.tick = Tick::from_raw(values[0]);
  sample.stream.resource = ResourceId::from_raw(values[1]);
  sample.stream.queue = QueueId::from_raw(values[2]);
  sample.seq = SampleSeq::from_raw(values[0]);
  sample.depth = values[3];
  sample.occupancy_bytes = values[4];
  sample.capacity_bytes = values[5];
  sample.ingress_rate_q16 = static_cast<RateQ16>(values[6]);
  sample.egress_rate_q16 = static_cast<RateQ16>(values[7]);
  sample.ingress_total = values[8];
  sample.egress_total = values[9];
  sample.drop_total = values[10];
  sample.mark_total = values[11];
  sample.flags = static_cast<SampleFlag>(static_cast<std::uint32_t>(values[12]));
  sample.resource_generation = ResourceGeneration::from_raw(static_cast<std::uint32_t>(values[13]));
  constexpr std::uint32_t kKnownFlags = 0x1FU;
  if ((values[12] & ~static_cast<std::uint64_t>(kKnownFlags)) != 0U) {
    return Status::failure(StatusCode::kCorrupt, "trace line carries undefined sample flags");
  }
  return validate_sample(sample);
}

std::string encode_trace(const std::vector<Sample>& samples) {
  std::string out;
  out.reserve(samples.size() * 96U);
  for (const Sample& sample : samples) {
    out.append(encode_line(sample));
    out.push_back('\n');
  }
  return out;
}

Status decode_trace(std::string_view text, std::vector<Sample>& samples, std::size_t max_samples) {
  samples.clear();
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const std::size_t newline = text.find('\n', offset);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = text.substr(offset, end - offset);
    if (!line.empty()) {
      if (samples.size() >= max_samples) {
        return Status::failure(StatusCode::kOversized, "trace exceeds the sample bound");
      }
      Sample sample;
      const Status status = decode_line(line, sample);
      if (!status.ok()) {
        return status;
      }
      samples.push_back(sample);
    }
    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1;
  }
  return Status{};
}

}  // namespace mbg::synthetic
