// Microburst Governor - per-stream microburst detector.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include "mbg/core/status.hpp"
#include "mbg/detect/classifier.hpp"
#include "mbg/detect/window.hpp"
#include "mbg/model/event.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/model/sample.hpp"

namespace mbg {

/// Detector state machine for a single evidence stream.
enum class DetectorState : std::uint8_t {
  kIdle = 0,        ///< no episode; onset rules are being watched
  kActive = 1,      ///< an episode is open and the release rules are not yet satisfied
  kRecovering = 2,  ///< the release rules fired; the episode stays open for the recovery window
};

[[nodiscard]] const char* to_string(DetectorState state) noexcept;

/// Outcome of one rule evaluation. This is the complete, self contained justification of the
/// classification the governor acts on.
struct DetectionDecision {
  Classification classification{Classification::kUnknown};
  EvidenceAuthority authority{EvidenceAuthority::kUnknown};
  SeverityClass severity{SeverityClass::kNone};
  GradeBreakdown grades{};

  StreamKey stream{};
  Tick tick{};
  EventId event{};
  ResourceGeneration resource_generation{};
  PolicyGeneration policy_generation{};
  std::uint64_t policy_digest{0};

  bool opened{false};
  bool updated{false};
  bool closed{false};
  bool fenced{false};
  bool suppressed_by_cooldown{false};
  CloseReason close_reason{CloseReason::kNone};

  EvidenceSummary evidence{};
  BurstMetrics metrics{};
  Explanation explanation{};

  std::uint64_t staleness_ticks{0};
  std::uint64_t onset_progress_ticks{0};
  std::uint64_t release_progress_ticks{0};
};

/// Deterministic detector for one stream.
///
/// The detector owns the evidence window, the onset/release sustain accounting, the episode identity
/// and the cooldown. It performs no intervention and never talks to any adjacent system: it turns
/// evidence into a classification with an explanation.
class Detector {
 public:
  Detector() = default;
  Detector(StreamKey key, Policy policy, PolicyGeneration generation);

  void configure(StreamKey key, Policy policy, PolicyGeneration generation);

  /// Installs a new policy. An open episode is fenced with kPolicyChanged because its thresholds no
  /// longer exist, and the evidence window is cleared because old samples were admitted under rules
  /// that no longer apply. Returns the fencing decision when an episode had to be closed.
  [[nodiscard]] std::optional<DetectionDecision> rearm(Policy policy, PolicyGeneration generation,
                                                      Tick now);

  /// Offers a sample and evaluates if the tick advanced.
  [[nodiscard]] Status ingest(const Sample& sample, DetectionDecision& out);

  /// Advances logical time without new evidence: staleness, sustain, release and episode ceilings are
  /// all time driven, so a stream that stops reporting is still evaluated.
  [[nodiscard]] Status advance(Tick now, DetectionDecision& out);

  /// Fences an open episode without evidence (authority loss, operator action, restart).
  [[nodiscard]] bool fence(CloseReason reason, Tick now, DetectionDecision& out);

  [[nodiscard]] DetectorState state() const noexcept { return state_; }
  [[nodiscard]] const StreamKey& stream() const noexcept { return key_; }
  [[nodiscard]] const EvidenceWindow& window() const noexcept { return window_; }
  [[nodiscard]] const BurstEvent& episode() const noexcept { return episode_; }
  [[nodiscard]] bool has_open_episode() const noexcept {
    return state_ == DetectorState::kActive || state_ == DetectorState::kRecovering;
  }
  [[nodiscard]] Tick last_evaluated() const noexcept { return last_evaluated_; }
  [[nodiscard]] bool has_evaluated() const noexcept { return has_evaluated_; }
  [[nodiscard]] Tick cooldown_until() const noexcept { return cooldown_until_; }
  [[nodiscard]] const DetectionDecision& last_decision() const noexcept { return last_decision_; }
  [[nodiscard]] const Policy& policy() const noexcept { return policy_; }
  [[nodiscard]] PolicyGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] ResourceGeneration resource_generation() const noexcept {
    return resource_generation_;
  }
  [[nodiscard]] WindowAccept last_accept() const noexcept { return last_accept_; }
  [[nodiscard]] bool in_cooldown(Tick now) const noexcept {
    return tick_after(now, cooldown_until_);
  }

 private:
  [[nodiscard]] DetectionDecision evaluate(Tick now);
  void open_episode(Tick now, DetectionDecision& decision);
  void update_episode(Tick now, const EvidenceWindow::Metrics& metrics, DetectionDecision& decision);
  void close_episode(CloseReason reason, Tick now, DetectionDecision& decision, bool fenced);

  StreamKey key_{};
  Policy policy_{};
  PolicyGeneration generation_{};
  ResourceGeneration resource_generation_{};
  EvidenceWindow window_{};
  DetectorState state_{DetectorState::kIdle};

  BurstEvent episode_{};
  DetectionDecision last_decision_{};
  Tick last_evaluated_{};
  bool has_evaluated_{false};

  /// Onset run accounting.
  ///
  /// A run begins when the depth threshold is first crossed and ends when the depth falls back
  /// below it. The rise-rate condition only has to be observed once inside the onset window; the
  /// depth condition has to persist for the sustain interval. Requiring the derivative itself to
  /// hold on every tick would make a step shaped burst undetectable, which is precisely the shape a
  /// microburst has.
  bool onset_run_active_{false};
  Tick onset_start_tick_{};
  std::uint64_t onset_run_ticks_{0};
  bool onset_rise_observed_{false};

  bool release_sustaining_{false};
  std::uint64_t release_progress_ticks_{0};
  Tick recovery_start_tick_{};

  Tick cooldown_until_{};

  bool inadmissible_since_set_{false};
  Tick inadmissible_since_{};

  WindowAccept last_accept_{WindowAccept::kAccepted};
};

}  // namespace mbg
