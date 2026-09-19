// Microburst Governor - tick source observation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/tick.hpp"

namespace mbg {

const char* TickSource::to_string(Observation observation) noexcept {
  switch (observation) {
    case Observation::kFirst: return "first";
    case Observation::kAdvance: return "advance";
    case Observation::kDuplicate: return "duplicate";
    case Observation::kReorder: return "reorder";
    case Observation::kRollover: return "rollover";
    case Observation::kDiscontinuity: return "discontinuity";
  }
  return "unknown";
}

TickSource::Step TickSource::observe(Tick tick, std::uint64_t reorder_tolerance_ticks) noexcept {
  if (!has_observation_) {
    has_observation_ = true;
    last_ = tick;
    accepted_ += 1;
    return Step{Observation::kFirst, 0};
  }

  const std::int64_t delta = tick_delta(last_, tick);
  if (delta > 0) {
    last_ = tick;
    accepted_ += 1;
    return Step{Observation::kAdvance, static_cast<std::uint64_t>(delta)};
  }
  if (delta == 0) {
    rejected_ += 1;
    return Step{Observation::kDuplicate, 0};
  }

  // Backwards. Distinguish a legitimate counter rollover from a reorder or a hard discontinuity.
  const std::uint64_t backwards = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
  const std::uint64_t distance_to_wrap = ~std::uint64_t{0} - last_.value() + 1U;
  if (backwards > distance_to_wrap && backwards - distance_to_wrap <= reorder_tolerance_ticks) {
    rollovers_ += 1;
    last_ = tick;
    accepted_ += 1;
    return Step{Observation::kRollover, backwards - distance_to_wrap};
  }
  if (backwards <= reorder_tolerance_ticks) {
    rejected_ += 1;
    return Step{Observation::kReorder, 0};
  }
  rejected_ += 1;
  return Step{Observation::kDiscontinuity, 0};
}

}  // namespace mbg
