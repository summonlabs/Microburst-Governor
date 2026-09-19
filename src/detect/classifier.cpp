// Microburst Governor - deterministic severity classification.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/detect/classifier.hpp"

#include "mbg/detect/quantities.hpp"

namespace mbg {

GradeBreakdown classify(const BurstMetrics& metrics, const EvidenceSummary& evidence,
                        const DetectionPolicy& policy, Tick tick,
                        Explanation& explanation) noexcept {
  GradeBreakdown breakdown{};

  const std::uint32_t occupancy_permille =
      compute_permille(metrics.peak_occupancy_bytes, metrics.capacity_bytes);
  breakdown.occupancy = policy.occupancy_permille_ladder.grade(occupancy_permille);
  if (breakdown.occupancy != SeverityClass::kNone) {
    const auto step = static_cast<std::size_t>(breakdown.occupancy) - 1U;
    explanation.add(ReasonCode::kOccupancyGradeCrossed,
                    static_cast<std::int64_t>(occupancy_permille),
                    static_cast<std::int64_t>(policy.occupancy_permille_ladder.thresholds[step]),
                    tick);
  }

  breakdown.depth = policy.depth_ladder.grade(metrics.peak_depth);
  if (breakdown.depth != SeverityClass::kNone) {
    const auto step = static_cast<std::size_t>(breakdown.depth) - 1U;
    explanation.add(ReasonCode::kDepthGradeCrossed, static_cast<std::int64_t>(metrics.peak_depth),
                    static_cast<std::int64_t>(policy.depth_ladder.thresholds[step]), tick);
  }

  breakdown.slope = policy.slope_ladder.grade_rate(metrics.peak_slope_q16);
  if (breakdown.slope != SeverityClass::kNone) {
    const auto step = static_cast<std::size_t>(breakdown.slope) - 1U;
    explanation.add(ReasonCode::kSlopeGradeCrossed,
                    static_cast<std::int64_t>(metrics.peak_slope_q16),
                    static_cast<std::int64_t>(policy.slope_ladder.thresholds[step]), tick);
  }

  breakdown.duration = policy.duration_ladder.grade(metrics.duration_ticks);
  if (breakdown.duration != SeverityClass::kNone) {
    const auto step = static_cast<std::size_t>(breakdown.duration) - 1U;
    explanation.add(ReasonCode::kDurationGradeCrossed,
                    static_cast<std::int64_t>(metrics.duration_ticks),
                    static_cast<std::int64_t>(policy.duration_ladder.thresholds[step]), tick);
  }

  breakdown.drops = policy.drop_ladder.grade(metrics.drop_delta);
  if (breakdown.drops != SeverityClass::kNone) {
    const auto step = static_cast<std::size_t>(breakdown.drops) - 1U;
    explanation.add(ReasonCode::kDropGradeCrossed, static_cast<std::int64_t>(metrics.drop_delta),
                    static_cast<std::int64_t>(policy.drop_ladder.thresholds[step]), tick);
  }

  SeverityClass overall = SeverityClass::kNone;
  overall = severity_max(overall, breakdown.occupancy);
  overall = severity_max(overall, breakdown.depth);
  overall = severity_max(overall, breakdown.slope);
  overall = severity_max(overall, breakdown.duration);
  overall = severity_max(overall, breakdown.drops);

  // A confirmed microburst is at least minor: the onset rules already proved a transient queue
  // explosion even when no severity ladder step was crossed.
  if (overall == SeverityClass::kNone) {
    overall = SeverityClass::kMinor;
  }
  // Without a known buffer capacity the occupancy dimension is unmeasurable, so the classification
  // never claims more than moderate on that basis.
  if (!evidence.capacity_known) {
    overall = severity_min(overall, SeverityClass::kModerate);
  }
  breakdown.overall = overall;
  return breakdown;
}

EvidenceSummary summarize(const EvidenceWindow::Metrics& metrics, const EvidenceWindow& window,
                          const DetectionPolicy& policy) noexcept {
  (void)policy;
  EvidenceSummary summary{};
  summary.window_start = metrics.window_start;
  summary.window_end = metrics.window_end;
  summary.samples = metrics.samples;
  summary.expected_samples = metrics.expected_samples;
  summary.completeness_permille = metrics.completeness_permille;
  summary.gaps = window.discontinuities();
  summary.reordered = window.reordered();
  summary.duplicates = window.duplicates();
  summary.counter_resets = window.counter_resets();
  summary.counter_rollovers = window.counter_rollovers();
  summary.dropped_samples = window.discontinuities() + window.reordered() + window.duplicates();
  summary.capacity_known = metrics.capacity_known;
  summary.capacity_bytes = metrics.capacity_bytes;
  summary.sparse = metrics.sparse;
  summary.synthetic = metrics.synthetic;
  summary.provenance = metrics.provenance;
  summary.provenance_count = metrics.provenance_count;
  return summary;
}

}  // namespace mbg
