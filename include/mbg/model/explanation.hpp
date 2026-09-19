// Microburst Governor - bounded decision explanation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mbg/limits.hpp"
#include "mbg/model/tick.hpp"

namespace mbg {

/// Every authoritative decision carries the rule outcomes that produced it. Reason codes are part
/// of the public contract (they appear in CLI output and in tests) and are never renumbered.
enum class ReasonCode : std::uint16_t {
  kNone = 0,

  // Evidence admissibility (1..19)
  kEvidenceAccepted = 1,
  kEvidenceInsufficientSamples = 2,
  kEvidenceIncomplete = 3,
  kEvidenceStale = 4,
  kEvidenceGap = 5,
  kEvidenceReordered = 6,
  kEvidenceDuplicate = 7,
  kEvidenceCounterReset = 8,
  kEvidenceCapacityChanged = 9,
  kEvidenceTickRollover = 10,
  kEvidenceDegradedAllowed = 11,
  kEvidenceDegradedRefused = 12,
  kEvidenceNoSamples = 13,

  // Threshold rules (20..39)
  kOnsetDepthCrossed = 20,
  kOnsetSlopeCrossed = 21,
  kOnsetSustainMet = 22,
  kReleaseDepthCrossed = 23,
  kReleaseSlopeCrossed = 24,
  kReleaseSustainMet = 25,
  kDurationExceeded = 26,
  kDropEvidencePresent = 27,
  kMarkEvidencePresent = 28,
  kIngressEgressImbalance = 29,
  kOccupancyGradeCrossed = 30,
  kDepthGradeCrossed = 31,
  kSlopeGradeCrossed = 32,
  kDurationGradeCrossed = 33,
  kDropGradeCrossed = 34,

  // Lifecycle (40..59)
  kEpisodeOpened = 40,
  kEpisodeMerged = 41,
  kEpisodeClosed = 42,
  kEpisodeFenced = 43,
  kCooldownActive = 44,
  kCooldownExpired = 45,
  kDuplicateSuppressed = 46,
  kEpisodeTimedOut = 47,
  kEpisodeRecovering = 48,

  // Intervention (60..79)
  kInterventionRequested = 60,
  kInterventionMagnitudeClamped = 61,
  kInterventionSuppressedByPolicy = 62,
  kInterventionSuppressedByCooldown = 63,
  kInterventionSuppressedByCapacity = 64,
  kInterventionSuppressedBySeverity = 65,
  kInterventionSuppressedByAuthority = 66,
  kInterventionExpired = 67,
  kInterventionRevokedStaleAuthority = 68,
  kInterventionRevokedPolicyChange = 69,
  kInterventionRevokedEpochChange = 70,
  kInterventionRevokedFenced = 71,
  kInterventionRevokedResourceGeneration = 72,

  // Authority (80..99)
  kAuthorityAuthoritative = 80,
  kAuthorityDegraded = 81,
  kAuthorityUnknown = 82,
  kAuthorityEpochAdvanced = 83,
  kAuthorityIncarnationChanged = 84,
  kAuthorityRestored = 85,
  kAuthorityFenced = 86,
};

[[nodiscard]] const char* to_string(ReasonCode code) noexcept;

/// One rule outcome. Operands are rule specific: observed value, threshold value and the tick at
/// which the rule was evaluated.
struct Reason {
  ReasonCode code{ReasonCode::kNone};
  std::int64_t observed{0};
  std::int64_t threshold{0};
  Tick tick{};

  friend constexpr bool operator==(const Reason&, const Reason&) noexcept = default;
};

/// Fixed capacity explanation. Overflow is recorded (truncated()) rather than growing without
/// bound, so a pathological evidence stream cannot inflate durable or emitted records.
class Explanation {
 public:
  static constexpr std::size_t kCapacity = limits::kMaxReasons;

  void add(ReasonCode code, std::int64_t observed, std::int64_t threshold, Tick tick) noexcept;
  void add(ReasonCode code, Tick tick) noexcept { add(code, 0, 0, tick); }
  void add(ReasonCode code) noexcept { add(code, 0, 0, Tick{}); }

  void clear() noexcept { count_ = 0; truncated_ = false; }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] const Reason& at(std::size_t index) const noexcept { return reasons_[index]; }
  [[nodiscard]] bool contains(ReasonCode code) const noexcept;

  /// Stable digest of the explanation content, used to prove deterministic classification.
  [[nodiscard]] std::uint64_t digest() const noexcept;

  /// Human readable rendering, bounded by limits::kMaxRenderedExplanationBytes.
  [[nodiscard]] std::string render() const;

  [[nodiscard]] bool operator==(const Explanation& other) const noexcept;

 private:
  std::array<Reason, kCapacity> reasons_{};
  std::size_t count_{0};
  bool truncated_{false};
};

}  // namespace mbg
