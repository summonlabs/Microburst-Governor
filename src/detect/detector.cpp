// Microburst Governor - per-stream microburst detector.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/detect/detector.hpp"

#include <algorithm>

#include "mbg/core/checked.hpp"
#include "mbg/detect/quantities.hpp"

namespace mbg {
namespace {

/// Non-negative distance between two ticks, clamped at zero for non-monotonic inputs.
[[nodiscard]] std::uint64_t forward_gap(Tick from, Tick to) noexcept {
  const std::int64_t delta = tick_delta(from, to);
  return delta <= 0 ? 0U : static_cast<std::uint64_t>(delta);
}

[[nodiscard]] Tick add_ticks_saturating(Tick base, std::uint64_t span) noexcept {
  const auto sum = add_checked<std::uint64_t>(base.value(), span);
  return Tick::from_raw(sum.has_value() ? *sum : ~std::uint64_t{0});
}

}  // namespace

const char* to_string(DetectorState state) noexcept {
  switch (state) {
    case DetectorState::kIdle: return "idle";
    case DetectorState::kActive: return "active";
    case DetectorState::kRecovering: return "recovering";
  }
  return "unknown";
}

Detector::Detector(StreamKey key, Policy policy, PolicyGeneration generation) {
  configure(key, policy, generation);
}

void Detector::configure(StreamKey key, Policy policy, PolicyGeneration generation) {
  key_ = key;
  policy_ = policy;
  generation_ = generation;
  window_.reset(static_cast<std::size_t>(policy_.detection.window_capacity));
  state_ = DetectorState::kIdle;
  episode_ = BurstEvent{};
  last_decision_ = DetectionDecision{};
  has_evaluated_ = false;
  last_evaluated_ = Tick{};
  onset_run_active_ = false;
  onset_start_tick_ = Tick{};
  onset_run_ticks_ = 0;
  onset_rise_observed_ = false;
  release_sustaining_ = false;
  release_progress_ticks_ = 0;
  recovery_start_tick_ = Tick{};
  cooldown_until_ = Tick{};
  inadmissible_since_set_ = false;
  inadmissible_since_ = Tick{};
}

std::optional<DetectionDecision> Detector::rearm(Policy policy, PolicyGeneration generation,
                                                Tick now) {
  std::optional<DetectionDecision> emit;
  if (has_open_episode()) {
    DetectionDecision decision;
    decision.stream = key_;
    decision.tick = now;
    decision.policy_generation = generation_;
    decision.policy_digest = policy_.digest();
    decision.resource_generation = resource_generation_;
    close_episode(CloseReason::kPolicyChanged, now, decision, true);
    decision.classification = Classification::kFenced;
    emit = decision;
  }
  policy_ = policy;
  generation_ = generation;
  window_.reset(static_cast<std::size_t>(policy_.detection.window_capacity));
  onset_run_active_ = false;
  onset_run_ticks_ = 0;
  onset_rise_observed_ = false;
  release_sustaining_ = false;
  release_progress_ticks_ = 0;
  state_ = DetectorState::kIdle;
  has_evaluated_ = false;
  last_evaluated_ = Tick{};
  inadmissible_since_set_ = false;
  return emit;
}

bool Detector::fence(CloseReason reason, Tick now, DetectionDecision& out) {
  if (!has_open_episode()) {
    return false;
  }
  out = DetectionDecision{};
  out.stream = key_;
  out.tick = now;
  out.policy_generation = generation_;
  out.policy_digest = policy_.digest();
  out.resource_generation = resource_generation_;
  close_episode(reason, now, out, true);
  out.classification = Classification::kFenced;
  out.evidence = last_decision_.evidence;
  return true;
}

Status Detector::ingest(const Sample& sample, DetectionDecision& out) {
  if (!(sample.stream == key_)) {
    return Status::failure(StatusCode::kInvalidArgument, "sample stream does not match detector");
  }
  const Status valid = validate_sample(sample);
  if (!valid.ok()) {
    return valid;
  }

  if (sample.resource_generation.valid() && resource_generation_.valid() &&
      sample.resource_generation != resource_generation_) {
    // A resource reconfiguration fences whatever was being measured: the quantities are no longer
    // comparable.
    if (has_open_episode()) {
      DetectionDecision fenced;
      fenced.stream = key_;
      fenced.tick = sample.tick;
      fenced.policy_generation = generation_;
      fenced.policy_digest = policy_.digest();
      fenced.resource_generation = resource_generation_;
      close_episode(CloseReason::kCapacityChanged, sample.tick, fenced, true);
      fenced.classification = Classification::kFenced;
      out = fenced;
      resource_generation_ = sample.resource_generation;
      window_.reset(static_cast<std::size_t>(policy_.detection.window_capacity));
      last_accept_ = WindowAccept::kCapacityChanged;
      return Status{};
    }
    resource_generation_ = sample.resource_generation;
    window_.reset(static_cast<std::size_t>(policy_.detection.window_capacity));
  } else if (sample.resource_generation.valid()) {
    resource_generation_ = sample.resource_generation;
  }

  last_accept_ = window_.offer(sample, policy_.detection.reorder_tolerance_ticks);
  out = evaluate(sample.tick);
  return Status{};
}

Status Detector::advance(Tick now, DetectionDecision& out) {
  out = evaluate(now);
  return Status{};
}

DetectionDecision Detector::evaluate(Tick now) {
  if (has_evaluated_ && !tick_after(last_evaluated_, now)) {
    // Time never runs backwards: a repeated or reordered evaluation returns the cached decision
    // instead of mutating authoritative state twice for the same instant.
    return last_decision_;
  }
  const std::uint64_t gap = has_evaluated_ ? forward_gap(last_evaluated_, now) : 0;
  last_evaluated_ = now;
  has_evaluated_ = true;

  const DetectionPolicy& dp = policy_.detection;
  DetectionDecision decision;
  decision.stream = key_;
  decision.tick = now;
  decision.policy_generation = generation_;
  decision.policy_digest = policy_.digest();
  decision.resource_generation = resource_generation_;

  const EvidenceWindow::Metrics metrics = window_.metrics(dp);
  decision.staleness_ticks = window_.staleness_ticks(now);
  decision.evidence = summarize(metrics, window_, dp);
  decision.onset_progress_ticks = onset_run_ticks_;
  decision.release_progress_ticks = release_progress_ticks_;

  const bool has_samples = metrics.samples > 0;
  const bool enough_samples =
      metrics.samples >= static_cast<std::uint64_t>(dp.min_samples_for_classification);
  const bool complete = metrics.completeness_permille >= dp.min_completeness_permille;
  const bool fresh = has_samples && decision.staleness_ticks <= dp.staleness_limit_ticks;
  const bool gaps_ok = metrics.max_gap_ticks <= dp.max_sample_gap_ticks;
  const bool stable = dp.allow_capacity_change_within_window || !metrics.capacity_changed;

  if (!has_samples) {
    decision.explanation.add(ReasonCode::kEvidenceNoSamples, now);
  } else {
    if (!enough_samples) {
      decision.explanation.add(ReasonCode::kEvidenceInsufficientSamples,
                               static_cast<std::int64_t>(metrics.samples),
                               static_cast<std::int64_t>(dp.min_samples_for_classification), now);
    }
    if (!complete) {
      decision.explanation.add(ReasonCode::kEvidenceIncomplete,
                               static_cast<std::int64_t>(metrics.completeness_permille),
                               static_cast<std::int64_t>(dp.min_completeness_permille), now);
    }
    if (!gaps_ok) {
      decision.explanation.add(ReasonCode::kEvidenceGap,
                               static_cast<std::int64_t>(metrics.max_gap_ticks),
                               static_cast<std::int64_t>(dp.max_sample_gap_ticks), now);
    }
    if (!stable) {
      decision.explanation.add(ReasonCode::kEvidenceCapacityChanged,
                               static_cast<std::int64_t>(metrics.capacity_bytes),
                               static_cast<std::int64_t>(metrics.capacity_bytes), now);
    }
    if (!fresh) {
      decision.explanation.add(ReasonCode::kEvidenceStale,
                               static_cast<std::int64_t>(decision.staleness_ticks),
                               static_cast<std::int64_t>(dp.staleness_limit_ticks), now);
    }
    if (window_.duplicates() != 0) {
      decision.explanation.add(ReasonCode::kEvidenceDuplicate,
                               static_cast<std::int64_t>(window_.duplicates()), 0, now);
    }
    if (window_.reordered() != 0) {
      decision.explanation.add(ReasonCode::kEvidenceReordered,
                               static_cast<std::int64_t>(window_.reordered()), 0, now);
    }
    if (window_.counter_resets() != 0) {
      decision.explanation.add(ReasonCode::kEvidenceCounterReset,
                               static_cast<std::int64_t>(window_.counter_resets()), 0, now);
    }
    if (window_.counter_rollovers() != 0) {
      decision.explanation.add(ReasonCode::kEvidenceTickRollover,
                               static_cast<std::int64_t>(window_.counter_rollovers()), 0, now);
    }
  }

  const bool authoritative = has_samples && enough_samples && complete && fresh && gaps_ok && stable;
  const bool degraded_allowed = !authoritative && dp.allow_degraded_classification && has_samples &&
                                fresh && stable && metrics.samples >= 2 &&
                                metrics.completeness_permille >= dp.degraded_min_completeness_permille;
  const bool admissible = authoritative || degraded_allowed;

  if (authoritative) {
    decision.explanation.add(ReasonCode::kEvidenceAccepted, now);
  } else if (degraded_allowed) {
    decision.explanation.add(ReasonCode::kEvidenceDegradedAllowed, now);
  } else {
    decision.explanation.add(ReasonCode::kEvidenceDegradedRefused, now);
  }

  if (!admissible) {
    decision.classification = Classification::kUnknown;
    decision.authority = EvidenceAuthority::kUnknown;
    decision.severity = SeverityClass::kNone;
    if (has_open_episode()) {
      if (!inadmissible_since_set_) {
        inadmissible_since_set_ = true;
        inadmissible_since_ = now;
      }
      // The grace is measured from the moment evidence stopped being usable, and additionally from
      // how far the newest sample has aged past the staleness limit. A window that is already far
      // beyond its staleness limit fences the episode at once instead of waiting for another
      // observation to accumulate.
      const std::uint64_t stale_excess =
          decision.staleness_ticks > dp.staleness_limit_ticks
              ? decision.staleness_ticks - dp.staleness_limit_ticks
              : 0U;
      const std::uint64_t held =
          std::max(forward_gap(inadmissible_since_, now), stale_excess);
      if (held <= dp.evidence_grace_ticks) {
        // Suspended, never positive: an open episode whose evidence is not admissible cannot make a
        // burst claim, but it is not closed on a single bad interval either.
        decision.classification = Classification::kRecovering;
        decision.authority = EvidenceAuthority::kUnknown;
        decision.event = episode_.id;
        decision.metrics = episode_.metrics;
        decision.severity = episode_.severity;
      } else {
        const CloseReason reason =
            !fresh ? CloseReason::kEvidenceStale
                   : (!stable ? CloseReason::kCapacityChanged : CloseReason::kEvidenceStale);
        close_episode(reason, now, decision, true);
        decision.classification = Classification::kFenced;
      }
    }
    last_decision_ = decision;
    return decision;
  }
  inadmissible_since_set_ = false;

  decision.authority = degraded_allowed ? EvidenceAuthority::kDegraded
                                        : EvidenceAuthority::kAuthoritative;

  // Onset rules.
  //
  // R1 depth: the queue is above the onset threshold.
  // R2 rise:  the queue rose at least onset_min_slope_q16 per tick, judged on the steepest of the
  //           last interval and the whole window, and only inside the onset window that follows the
  //           first crossing of R1. A queue that merely sits high and later drifts upward is not a
  //           microburst.
  // R3 hold:  R1 has held continuously for at least onset_sustain_ticks.
  const bool depth_ok = metrics.latest_depth >= dp.onset_min_depth;
  const RateQ16 steepest_slope =
      metrics.recent_slope_q16 > metrics.window_slope_q16 ? metrics.recent_slope_q16
                                                          : metrics.window_slope_q16;
  const bool slope_ok = steepest_slope >= dp.onset_min_slope_q16;
  if (depth_ok) {
    decision.explanation.add(ReasonCode::kOnsetDepthCrossed,
                             static_cast<std::int64_t>(metrics.latest_depth),
                             static_cast<std::int64_t>(dp.onset_min_depth), now);
  }
  if (depth_ok) {
    if (!onset_run_active_) {
      onset_run_active_ = true;
      onset_start_tick_ = now;
      onset_run_ticks_ = 0;
      onset_rise_observed_ = false;
    } else {
      onset_run_ticks_ = saturating_add<std::uint64_t>(onset_run_ticks_, gap);
    }
    if (slope_ok && onset_run_ticks_ <= dp.onset_slope_window_ticks) {
      onset_rise_observed_ = true;
    }
  } else {
    onset_run_active_ = false;
    onset_run_ticks_ = 0;
    onset_rise_observed_ = false;
  }
  if (onset_rise_observed_) {
    decision.explanation.add(ReasonCode::kOnsetSlopeCrossed,
                             static_cast<std::int64_t>(steepest_slope),
                             static_cast<std::int64_t>(dp.onset_min_slope_q16), now);
  }
  const bool onset_confirmed =
      onset_run_active_ && onset_rise_observed_ && onset_run_ticks_ >= dp.onset_sustain_ticks;
  if (onset_confirmed) {
    decision.explanation.add(ReasonCode::kOnsetSustainMet,
                             static_cast<std::int64_t>(onset_run_ticks_),
                             static_cast<std::int64_t>(dp.onset_sustain_ticks), now);
  }

  const bool release_depth_ok = metrics.latest_depth <= dp.release_depth;
  const bool release_slope_ok = metrics.recent_slope_q16 <= dp.release_slope_q16;
  const bool release_now = release_depth_ok && release_slope_ok;
  if (release_now && has_open_episode()) {
    if (!release_sustaining_) {
      release_sustaining_ = true;
      release_progress_ticks_ = 0;
    } else {
      release_progress_ticks_ = saturating_add<std::uint64_t>(release_progress_ticks_, gap);
    }
    decision.explanation.add(ReasonCode::kReleaseDepthCrossed,
                             static_cast<std::int64_t>(metrics.latest_depth),
                             static_cast<std::int64_t>(dp.release_depth), now);
  } else {
    release_sustaining_ = false;
    release_progress_ticks_ = 0;
  }
  const bool release_confirmed = release_now && release_sustaining_ &&
                                 release_progress_ticks_ >= dp.release_sustain_ticks;
  if (release_confirmed) {
    decision.explanation.add(ReasonCode::kReleaseSustainMet,
                             static_cast<std::int64_t>(release_progress_ticks_),
                             static_cast<std::int64_t>(dp.release_sustain_ticks), now);
  }

  const bool cooldown_ok = !in_cooldown(now);
  if (!cooldown_ok) {
    decision.suppressed_by_cooldown = true;
    decision.explanation.add(ReasonCode::kCooldownActive,
                             static_cast<std::int64_t>(now.value()),
                             static_cast<std::int64_t>(cooldown_until_.value()), now);
  }
  decision.onset_progress_ticks = onset_run_ticks_;
  decision.release_progress_ticks = release_progress_ticks_;

  switch (state_) {
    case DetectorState::kIdle: {
      if (onset_confirmed && cooldown_ok) {
        open_episode(now, decision);
        update_episode(now, metrics, decision);
        decision.classification = degraded_allowed ? Classification::kDegradedMicroburst
                                                   : Classification::kMicroburst;
      } else if (onset_run_active_) {
        decision.classification = Classification::kCandidate;
      } else {
        decision.classification = Classification::kQuiescent;
      }
      break;
    }
    case DetectorState::kActive: {
      update_episode(now, metrics, decision);
      if (release_confirmed) {
        state_ = DetectorState::kRecovering;
        recovery_start_tick_ = now;
        decision.explanation.add(ReasonCode::kEpisodeRecovering, now);
        decision.classification = Classification::kRecovering;
      } else {
        decision.classification = degraded_allowed ? Classification::kDegradedMicroburst
                                                   : Classification::kMicroburst;
      }
      break;
    }
    case DetectorState::kRecovering: {
      if (onset_run_active_) {
        state_ = DetectorState::kActive;
        release_sustaining_ = false;
        release_progress_ticks_ = 0;
        decision.explanation.add(ReasonCode::kEpisodeMerged, now);
        update_episode(now, metrics, decision);
        decision.classification = degraded_allowed ? Classification::kDegradedMicroburst
                                                   : Classification::kMicroburst;
      } else if (forward_gap(recovery_start_tick_, now) >= dp.cooldown_ticks) {
        update_episode(now, metrics, decision);
        close_episode(CloseReason::kReleaseConfirmed, now, decision, false);
        decision.classification = Classification::kQuiescent;
      } else {
        update_episode(now, metrics, decision);
        decision.classification = Classification::kRecovering;
      }
      break;
    }
  }

  if (has_open_episode() && episode_.metrics.duration_ticks >= dp.max_episode_ticks) {
    decision.explanation.add(ReasonCode::kEpisodeTimedOut,
                             static_cast<std::int64_t>(episode_.metrics.duration_ticks),
                             static_cast<std::int64_t>(dp.max_episode_ticks), now);
    close_episode(CloseReason::kEpisodeTimeout, now, decision, false);
    decision.classification = Classification::kQuiescent;
  }

  last_decision_ = decision;
  return decision;
}

void Detector::open_episode(Tick now, DetectionDecision& decision) {
  episode_ = BurstEvent{};
  episode_.stream = key_;
  episode_.resource_generation = resource_generation_;
  episode_.id = make_event_id(key_, onset_start_tick_);
  episode_.lifecycle = EventLifecycle::kOpen;
  episode_.policy_id = policy_.id;
  episode_.policy_generation = generation_;
  episode_.policy_digest = policy_.digest();
  episode_.metrics.onset_tick = onset_start_tick_;
  episode_.metrics.peak_tick = now;
  episode_.metrics.last_update_tick = now;
  episode_.metrics.end_tick = now;
  episode_.revision = 1;
  state_ = DetectorState::kActive;
  release_sustaining_ = false;
  release_progress_ticks_ = 0;
  decision.opened = true;
  decision.event = episode_.id;
  decision.explanation.add(ReasonCode::kEpisodeOpened,
                           static_cast<std::int64_t>(onset_start_tick_.value()),
                           static_cast<std::int64_t>(now.value()), now);
}

void Detector::update_episode(Tick now, const EvidenceWindow::Metrics& metrics,
                              DetectionDecision& decision) {
  if (!has_open_episode()) {
    return;
  }
  const bool new_peak = metrics.peak_depth > episode_.metrics.peak_depth;
  episode_.metrics.peak_depth = std::max(episode_.metrics.peak_depth, metrics.peak_depth);
  episode_.metrics.peak_occupancy_bytes =
      std::max(episode_.metrics.peak_occupancy_bytes, metrics.peak_occupancy_bytes);
  episode_.metrics.peak_slope_q16 = std::max(episode_.metrics.peak_slope_q16, metrics.recent_slope_q16);
  episode_.metrics.peak_imbalance_q16 =
      std::max(episode_.metrics.peak_imbalance_q16, metrics.imbalance_q16);
  episode_.metrics.capacity_bytes = metrics.capacity_bytes;
  episode_.metrics.drop_delta = std::max(episode_.metrics.drop_delta, metrics.drop_delta);
  episode_.metrics.mark_delta = std::max(episode_.metrics.mark_delta, metrics.mark_delta);
  episode_.metrics.last_update_tick = now;
  episode_.metrics.end_tick = now;
  episode_.metrics.duration_ticks = forward_gap(episode_.metrics.onset_tick, now);
  if (new_peak) {
    episode_.metrics.peak_tick = now;
  }
  episode_.evidence = decision.evidence;
  const GradeBreakdown breakdown =
      classify(episode_.metrics, decision.evidence, policy_.detection, now, decision.explanation);
  SeverityClass severity = breakdown.overall;
  if (decision.authority == EvidenceAuthority::kDegraded) {
    // Degraded evidence can never claim more than the policy cap allows, whatever the metrics say.
    const SeverityClass capped = severity_min(severity, policy_.detection.degraded_severity_cap);
    if (capped != severity) {
      decision.explanation.add(ReasonCode::kAuthorityDegraded,
                               static_cast<std::int64_t>(severity),
                               static_cast<std::int64_t>(capped), now);
      severity = capped;
    }
  }
  episode_.severity = severity;
  episode_.authority = decision.authority;
  episode_.revision = saturating_add<std::uint32_t>(episode_.revision, 1U);

  decision.metrics = episode_.metrics;
  decision.severity = severity;
  decision.event = episode_.id;
  decision.updated = true;
  decision.grades = breakdown;
  decision.grades.overall = severity;
}

void Detector::close_episode(CloseReason reason, Tick now, DetectionDecision& decision,
                             bool fenced) {
  episode_.lifecycle = fenced ? EventLifecycle::kFenced : EventLifecycle::kClosed;
  episode_.close_reason = reason;
  episode_.closed_tick = now;
  episode_.metrics.end_tick = now;
  episode_.metrics.duration_ticks = forward_gap(episode_.metrics.onset_tick, now);
  episode_.revision = saturating_add<std::uint32_t>(episode_.revision, 1U);
  state_ = DetectorState::kIdle;
  cooldown_until_ = add_ticks_saturating(now, policy_.detection.cooldown_ticks);
  onset_run_active_ = false;
  onset_run_ticks_ = 0;
  onset_rise_observed_ = false;
  release_sustaining_ = false;
  release_progress_ticks_ = 0;
  inadmissible_since_set_ = false;

  decision.closed = true;
  decision.fenced = fenced;
  decision.event = episode_.id;
  decision.close_reason = reason;
  decision.metrics = episode_.metrics;
  decision.severity = episode_.severity;
  if (decision.evidence.samples == 0) {
    decision.evidence = last_decision_.evidence;
  }
  decision.explanation.add(fenced ? ReasonCode::kEpisodeFenced : ReasonCode::kEpisodeClosed,
                           static_cast<std::int64_t>(now.value()),
                           static_cast<std::int64_t>(episode_.metrics.duration_ticks), now);
  if (fenced) {
    decision.explanation.add(ReasonCode::kAuthorityFenced, now);
  }
}

}  // namespace mbg
