// Microburst Governor - bounded evidence window.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/detect/window.hpp"

#include <algorithm>

#include "mbg/core/checked.hpp"
#include "mbg/detect/quantities.hpp"

namespace mbg {

const char* to_string(WindowAccept accept) noexcept {
  switch (accept) {
    case WindowAccept::kAccepted: return "accepted";
    case WindowAccept::kDuplicate: return "duplicate";
    case WindowAccept::kReordered: return "reordered";
    case WindowAccept::kDiscontinuity: return "discontinuity";
    case WindowAccept::kCapacityChanged: return "capacity-changed";
  }
  return "unknown";
}

EvidenceWindow::EvidenceWindow(std::size_t capacity) { reset(capacity); }

void EvidenceWindow::reset(std::size_t capacity) {
  const std::size_t bounded = std::min(capacity, limits::kMaxWindowCapacity);
  capacity_ = std::max<std::size_t>(bounded, limits::kMinWindowCapacity);
  samples_.assign(capacity_, Sample{});
  head_ = 0;
  size_ = 0;
  clear();
}

void EvidenceWindow::clear() noexcept {
  head_ = 0;
  size_ = 0;
  duplicates_ = 0;
  reordered_ = 0;
  discontinuities_ = 0;
  counter_resets_ = 0;
  counter_rollovers_ = 0;
  capacity_changes_ = 0;
  sparse_samples_ = 0;
  synthetic_samples_ = 0;
  accepted_total_ = 0;
  max_observed_gap_ = 0;
  last_gap_ = 0;
  tick_source_ = TickSource{};
}

const Sample& EvidenceWindow::at(std::size_t index) const noexcept {
  const std::size_t slot = (head_ + index) % capacity_;
  return samples_[slot];
}

WindowAccept EvidenceWindow::offer(const Sample& sample, std::uint64_t reorder_tolerance_ticks) {
  if (has_flag(sample.flags, SampleFlag::kSparse)) {
    sparse_samples_ += 1;
  }
  if (has_flag(sample.flags, SampleFlag::kSynthetic)) {
    synthetic_samples_ += 1;
  }
  if (has_flag(sample.flags, SampleFlag::kCounterReset)) {
    counter_resets_ += 1;
  }
  if (!empty() && has_flag(sample.flags, SampleFlag::kCapacityChange)) {
    capacity_changes_ += 1;
  }

  const TickSource::Step step = tick_source_.observe(sample.tick, reorder_tolerance_ticks);
  if (step.observation == TickSource::Observation::kDuplicate) {
    duplicates_ += 1;
    return WindowAccept::kDuplicate;
  }
  if (step.observation == TickSource::Observation::kReorder) {
    reordered_ += 1;
    return WindowAccept::kReordered;
  }
  if (step.observation == TickSource::Observation::kDiscontinuity) {
    discontinuities_ += 1;
    return WindowAccept::kDiscontinuity;
  }
  if (step.observation == TickSource::Observation::kRollover) {
    counter_rollovers_ += 1;
  }

  last_gap_ = step.forward_ticks;
  if (step.observation != TickSource::Observation::kFirst) {
    max_observed_gap_ = std::max(max_observed_gap_, step.forward_ticks);
  }

  const bool capacity_changed =
      !empty() && at(size_ - 1).capacity_bytes != sample.capacity_bytes &&
      sample.capacity_bytes != 0 && at(size_ - 1).capacity_bytes != 0;

  if (size_ == capacity_) {
    samples_[head_] = sample;
    head_ = (head_ + 1) % capacity_;
  } else {
    samples_[(head_ + size_) % capacity_] = sample;
    size_ += 1;
  }
  accepted_total_ += 1;

  if (capacity_changed) {
    capacity_changes_ += 1;
    return WindowAccept::kCapacityChanged;
  }
  return WindowAccept::kAccepted;
}

EvidenceWindow::Metrics EvidenceWindow::metrics(const DetectionPolicy& policy) const noexcept {
  Metrics m{};
  if (size_ == 0) {
    return m;
  }

  const Sample& newest_sample = newest();
  m.samples = static_cast<std::uint64_t>(size_);
  m.window_start = oldest().tick;
  m.window_end = newest_sample.tick;
  m.span_ticks = static_cast<std::uint64_t>(tick_delta(m.window_start, m.window_end));

  // Expected samples over the observed span at the nominal cadence, saturated at a bounded ceiling so
  // a hostile tick span cannot inflate the denominator beyond the window bound.
  const std::uint64_t interval = std::max<std::uint64_t>(policy.nominal_sample_interval_ticks, 1U);
  std::uint64_t expected = m.span_ticks / interval;
  expected = saturating_add<std::uint64_t>(expected, 1U);
  const std::uint64_t ceiling =
      saturating_mul<std::uint64_t>(static_cast<std::uint64_t>(capacity_), 64U);
  m.expected_samples = std::min(expected, std::max<std::uint64_t>(ceiling, 1U));

  const std::uint64_t ratio = saturating_mul<std::uint64_t>(m.samples, 1000U);
  const std::uint64_t completeness =
      m.expected_samples == 0 ? 1000U : std::min<std::uint64_t>(1000U, ratio / m.expected_samples);
  m.completeness_permille = static_cast<std::uint32_t>(completeness);

  m.latest_depth = newest_sample.depth;
  m.latest_occupancy_bytes = newest_sample.occupancy_bytes;
  m.peak_depth = newest_sample.depth;
  m.peak_occupancy_bytes = newest_sample.occupancy_bytes;
  m.capacity_bytes = newest_sample.capacity_bytes;
  m.capacity_known = newest_sample.capacity_bytes != 0;
  m.synthetic = has_flag(newest_sample.flags, SampleFlag::kSynthetic);
  m.sparse = has_flag(newest_sample.flags, SampleFlag::kSparse);
  m.provenance = newest_sample.provenance;
  m.provenance_count = newest_sample.provenance.valid() ? 1U : 0U;

  std::uint64_t capacity_variants = 0;
  std::uint64_t first_capacity = 0;
  for (std::size_t i = 0; i < size_; ++i) {
    const Sample& s = at(i);
    m.peak_depth = std::max(m.peak_depth, s.depth);
    m.peak_occupancy_bytes = std::max(m.peak_occupancy_bytes, s.occupancy_bytes);
    if (s.provenance.valid() && s.provenance != m.provenance) {
      m.provenance_count += 1;
    }
    if (s.capacity_bytes != 0) {
      if (first_capacity == 0) {
        first_capacity = s.capacity_bytes;
      } else if (s.capacity_bytes != first_capacity) {
        capacity_variants += 1;
      }
    }
    if (has_flag(s.flags, SampleFlag::kSparse)) {
      m.sparse = true;
    }
    if (has_flag(s.flags, SampleFlag::kSynthetic)) {
      m.synthetic = true;
    }
  }
  m.capacity_changed = capacity_variants > 0 || capacity_changes_ > 0;

  // The gap that matters for admissibility is the largest gap between samples that are still inside
  // the window. A single historical gap must not poison the stream forever: once the samples that
  // straddled it have aged out, the window is contiguous again.
  std::uint64_t window_max_gap = 0;
  for (std::size_t i = 1; i < size_; ++i) {
    const std::int64_t step = tick_delta(at(i - 1).tick, at(i).tick);
    if (step > 0) {
      window_max_gap = std::max(window_max_gap, static_cast<std::uint64_t>(step));
    }
  }
  m.max_gap_ticks = window_max_gap;

  const std::int64_t depth_delta =
      static_cast<std::int64_t>(newest_sample.depth) - static_cast<std::int64_t>(oldest().depth);
  m.window_slope_q16 = compute_slope_q16(depth_delta, m.span_ticks);

  if (size_ >= 2) {
    const Sample& prior = at(size_ - 2);
    const std::uint64_t delta_ticks =
        static_cast<std::uint64_t>(tick_delta(prior.tick, newest_sample.tick));
    const std::int64_t recent_delta =
        static_cast<std::int64_t>(newest_sample.depth) - static_cast<std::int64_t>(prior.depth);
    m.recent_slope_q16 = compute_slope_q16(recent_delta, delta_ticks);
  }

  m.imbalance_q16 = newest_sample.ingress_rate_q16 - newest_sample.egress_rate_q16;
  m.drop_delta = window_counter_delta(oldest().drop_total, newest_sample.drop_total,
                                      has_flag(newest_sample.flags, SampleFlag::kCounterReset));
  m.mark_delta = window_counter_delta(oldest().mark_total, newest_sample.mark_total,
                                      has_flag(newest_sample.flags, SampleFlag::kCounterReset));
  m.ingress_delta = window_counter_delta(oldest().ingress_total, newest_sample.ingress_total,
                                         has_flag(newest_sample.flags, SampleFlag::kCounterReset));
  m.egress_delta = window_counter_delta(oldest().egress_total, newest_sample.egress_total,
                                        has_flag(newest_sample.flags, SampleFlag::kCounterReset));
  return m;
}

std::uint64_t EvidenceWindow::staleness_ticks(Tick now) const noexcept {
  if (size_ == 0) {
    return 0;
  }
  const std::int64_t delta = tick_delta(newest().tick, now);
  return delta <= 0 ? 0U : static_cast<std::uint64_t>(delta);
}

}  // namespace mbg
