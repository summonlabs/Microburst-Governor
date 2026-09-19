// Microburst Governor - evidence sample validation and counter resolution.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/sample.hpp"

#include "mbg/core/hash.hpp"

namespace mbg {

const char* to_string(SampleFlag flag) noexcept {
  switch (flag) {
    case SampleFlag::kNone: return "none";
    case SampleFlag::kCounterReset: return "counter-reset";
    case SampleFlag::kCapacityChange: return "capacity-change";
    case SampleFlag::kSparse: return "sparse";
    case SampleFlag::kSynthetic: return "synthetic";
    case SampleFlag::kPartialInterval: return "partial-interval";
  }
  return "unknown";
}

std::uint64_t StreamKey::hash() const noexcept {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  h = hash_combine(h, resource.raw());
  h = hash_combine(h, queue.raw());
  h = hash_combine(h, path.valid() ? path.raw() : 0U);
  return h;
}

Status validate_sample(const Sample& sample) noexcept {
  if (!sample.stream.valid()) {
    return Status::failure(StatusCode::kInvalidArgument, "sample stream identity is incomplete");
  }
  if (static_cast<std::size_t>(sample.affected_class_count) > sample.affected_classes.size()) {
    return Status::failure(StatusCode::kInvalidArgument, "affected class count exceeds capacity");
  }
  if (sample.capacity_bytes != 0 && sample.occupancy_bytes > sample.capacity_bytes &&
      !has_flag(sample.flags, SampleFlag::kCapacityChange)) {
    return Status::failure(StatusCode::kConflict,
                           "occupancy exceeds capacity without a declared capacity change");
  }
  if (sample.ingress_rate_q16 < 0 || sample.egress_rate_q16 < 0) {
    return Status::failure(StatusCode::kInvalidArgument, "negative rate is not representable");
  }
  if (sample.depth > (1ULL << 62) || sample.occupancy_bytes > (1ULL << 62)) {
    return Status::failure(StatusCode::kOutOfRange, "depth or occupancy exceeds supported range");
  }
  for (std::uint8_t i = 0; i < sample.affected_class_count; ++i) {
    if (!sample.affected_classes[i].valid()) {
      return Status::failure(StatusCode::kInvalidArgument, "affected class entry is invalid");
    }
  }
  return Status{};
}

CounterResolution resolve_counter(std::uint64_t previous, std::uint64_t current,
                                  bool reset_declared) noexcept {
  if (reset_declared) {
    return CounterResolution{0, CounterDisposition::kDeclaredReset, false};
  }
  if (current >= previous) {
    return CounterResolution{current - previous, CounterDisposition::kAdvance, true};
  }
  // Backwards: a modular forward step below 2^63 is a rollover, anything else is a regression.
  const std::uint64_t modular = current - previous;  // two's complement modular distance
  if (modular < (1ULL << 63)) {
    return CounterResolution{modular, CounterDisposition::kRollover, true};
  }
  return CounterResolution{0, CounterDisposition::kRegression, false};
}

CounterResolution CounterCursor::observe(std::uint64_t current, bool reset_declared) noexcept {
  if (!has_previous) {
    has_previous = true;
    previous = current;
    if (reset_declared) {
      declared_resets += 1;
    }
    return CounterResolution{0, CounterDisposition::kFirstObservation, false};
  }
  const CounterResolution resolution = resolve_counter(previous, current, reset_declared);
  previous = current;
  switch (resolution.disposition) {
    case CounterDisposition::kRollover: rollovers += 1; break;
    case CounterDisposition::kRegression: regressions += 1; break;
    case CounterDisposition::kDeclaredReset: declared_resets += 1; break;
    case CounterDisposition::kAdvance:
    case CounterDisposition::kFirstObservation:
      break;
  }
  return resolution;
}

}  // namespace mbg
