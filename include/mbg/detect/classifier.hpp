// Microburst Governor - deterministic severity classification.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mbg/detect/window.hpp"
#include "mbg/model/event.hpp"
#include "mbg/model/explanation.hpp"
#include "mbg/model/policy.hpp"

namespace mbg {

/// Severity contributed by each independent dimension.
struct GradeBreakdown {
  SeverityClass occupancy{SeverityClass::kNone};
  SeverityClass depth{SeverityClass::kNone};
  SeverityClass slope{SeverityClass::kNone};
  SeverityClass duration{SeverityClass::kNone};
  SeverityClass drops{SeverityClass::kNone};
  SeverityClass overall{SeverityClass::kNone};
};

/// Grades an episode.
///
/// Each dimension is graded independently against its policy ladder and the overall grade is the
/// maximum. The result is a pure function of the metrics, the evidence summary and the policy, so
/// two runs over identical evidence always agree. Every crossed threshold is recorded in the
/// explanation.
[[nodiscard]] GradeBreakdown classify(const BurstMetrics& metrics, const EvidenceSummary& evidence,
                                      const DetectionPolicy& policy, Tick tick,
                                      Explanation& explanation) noexcept;

/// Projects window metrics and refusal counters into the evidence summary a decision is bound to.
[[nodiscard]] EvidenceSummary summarize(const EvidenceWindow::Metrics& metrics,
                                        const EvidenceWindow& window,
                                        const DetectionPolicy& policy) noexcept;

}  // namespace mbg
