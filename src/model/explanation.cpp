// Microburst Governor - bounded decision explanation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/explanation.hpp"

#include "mbg/core/hash.hpp"

namespace mbg {
namespace {

}  // namespace

const char* to_string(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::kNone: return "none";

    case ReasonCode::kEvidenceAccepted: return "evidence-accepted";
    case ReasonCode::kEvidenceInsufficientSamples: return "evidence-insufficient-samples";
    case ReasonCode::kEvidenceIncomplete: return "evidence-incomplete";
    case ReasonCode::kEvidenceStale: return "evidence-stale";
    case ReasonCode::kEvidenceGap: return "evidence-gap";
    case ReasonCode::kEvidenceReordered: return "evidence-reordered";
    case ReasonCode::kEvidenceDuplicate: return "evidence-duplicate";
    case ReasonCode::kEvidenceCounterReset: return "evidence-counter-reset";
    case ReasonCode::kEvidenceCapacityChanged: return "evidence-capacity-changed";
    case ReasonCode::kEvidenceTickRollover: return "evidence-tick-rollover";
    case ReasonCode::kEvidenceDegradedAllowed: return "evidence-degraded-allowed";
    case ReasonCode::kEvidenceDegradedRefused: return "evidence-degraded-refused";
    case ReasonCode::kEvidenceNoSamples: return "evidence-no-samples";

    case ReasonCode::kOnsetDepthCrossed: return "onset-depth-crossed";
    case ReasonCode::kOnsetSlopeCrossed: return "onset-slope-crossed";
    case ReasonCode::kOnsetSustainMet: return "onset-sustain-met";
    case ReasonCode::kReleaseDepthCrossed: return "release-depth-crossed";
    case ReasonCode::kReleaseSlopeCrossed: return "release-slope-crossed";
    case ReasonCode::kReleaseSustainMet: return "release-sustain-met";
    case ReasonCode::kDurationExceeded: return "duration-exceeded";
    case ReasonCode::kDropEvidencePresent: return "drop-evidence-present";
    case ReasonCode::kMarkEvidencePresent: return "mark-evidence-present";
    case ReasonCode::kIngressEgressImbalance: return "ingress-egress-imbalance";
    case ReasonCode::kOccupancyGradeCrossed: return "occupancy-grade-crossed";
    case ReasonCode::kDepthGradeCrossed: return "depth-grade-crossed";
    case ReasonCode::kSlopeGradeCrossed: return "slope-grade-crossed";
    case ReasonCode::kDurationGradeCrossed: return "duration-grade-crossed";
    case ReasonCode::kDropGradeCrossed: return "drop-grade-crossed";

    case ReasonCode::kEpisodeOpened: return "episode-opened";
    case ReasonCode::kEpisodeMerged: return "episode-merged";
    case ReasonCode::kEpisodeClosed: return "episode-closed";
    case ReasonCode::kEpisodeFenced: return "episode-fenced";
    case ReasonCode::kCooldownActive: return "cooldown-active";
    case ReasonCode::kCooldownExpired: return "cooldown-expired";
    case ReasonCode::kDuplicateSuppressed: return "duplicate-suppressed";
    case ReasonCode::kEpisodeTimedOut: return "episode-timed-out";
    case ReasonCode::kEpisodeRecovering: return "episode-recovering";

    case ReasonCode::kInterventionRequested: return "intervention-requested";
    case ReasonCode::kInterventionMagnitudeClamped: return "intervention-magnitude-clamped";
    case ReasonCode::kInterventionSuppressedByPolicy: return "intervention-suppressed-policy";
    case ReasonCode::kInterventionSuppressedByCooldown: return "intervention-suppressed-cooldown";
    case ReasonCode::kInterventionSuppressedByCapacity: return "intervention-suppressed-capacity";
    case ReasonCode::kInterventionSuppressedBySeverity: return "intervention-suppressed-severity";
    case ReasonCode::kInterventionSuppressedByAuthority: return "intervention-suppressed-authority";
    case ReasonCode::kInterventionExpired: return "intervention-expired";
    case ReasonCode::kInterventionRevokedStaleAuthority:
      return "intervention-revoked-stale-authority";
    case ReasonCode::kInterventionRevokedPolicyChange: return "intervention-revoked-policy-change";
    case ReasonCode::kInterventionRevokedEpochChange: return "intervention-revoked-epoch-change";
    case ReasonCode::kInterventionRevokedFenced: return "intervention-revoked-fenced";
    case ReasonCode::kInterventionRevokedResourceGeneration:
      return "intervention-revoked-resource-generation";

    case ReasonCode::kAuthorityAuthoritative: return "authority-authoritative";
    case ReasonCode::kAuthorityDegraded: return "authority-degraded";
    case ReasonCode::kAuthorityUnknown: return "authority-unknown";
    case ReasonCode::kAuthorityEpochAdvanced: return "authority-epoch-advanced";
    case ReasonCode::kAuthorityIncarnationChanged: return "authority-incarnation-changed";
    case ReasonCode::kAuthorityRestored: return "authority-restored";
    case ReasonCode::kAuthorityFenced: return "authority-fenced";
  }
  return "unknown";
}

void Explanation::add(ReasonCode code, std::int64_t observed, std::int64_t threshold,
                      Tick tick) noexcept {
  if (count_ >= kCapacity) {
    truncated_ = true;
    return;
  }
  Reason& slot = reasons_[count_];
  slot.code = code;
  slot.observed = observed;
  slot.threshold = threshold;
  slot.tick = tick;
  count_ += 1;
}

bool Explanation::contains(ReasonCode code) const noexcept {
  for (std::size_t i = 0; i < count_; ++i) {
    if (reasons_[i].code == code) {
      return true;
    }
  }
  return false;
}

std::uint64_t Explanation::digest() const noexcept {
  std::uint64_t h = 0xC0FFEE123456789ULL;
  h = hash_combine(h, static_cast<std::uint64_t>(count_));
  for (std::size_t i = 0; i < count_; ++i) {
    h = hash_combine(h, static_cast<std::uint64_t>(reasons_[i].code));
    h = hash_combine(h, static_cast<std::uint64_t>(reasons_[i].observed));
    h = hash_combine(h, static_cast<std::uint64_t>(reasons_[i].threshold));
    h = hash_combine(h, reasons_[i].tick.value());
  }
  h = hash_combine(h, truncated_ ? 1U : 0U);
  return h;
}

std::string Explanation::render() const {
  std::string out;
  out.reserve(256);
  for (std::size_t i = 0; i < count_; ++i) {
    const Reason& reason = reasons_[i];
    if (i != 0) {
      out.push_back(';');
    }
    out.append(to_string(reason.code));
    out.push_back('(');
    out.append(std::to_string(reason.observed));
    out.push_back('/');
    out.append(std::to_string(reason.threshold));
    out.push_back('@');
    out.append(std::to_string(reason.tick.value()));
    out.push_back(')');
    if (out.size() >= limits::kMaxRenderedExplanationBytes) {
      out.resize(limits::kMaxRenderedExplanationBytes);
      break;
    }
  }
  if (truncated_) {
    out.append(";+truncated");
    if (out.size() > limits::kMaxRenderedExplanationBytes) {
      out.resize(limits::kMaxRenderedExplanationBytes);
    }
  }
  return out;
}

bool Explanation::operator==(const Explanation& other) const noexcept {
  if (count_ != other.count_ || truncated_ != other.truncated_) {
    return false;
  }
  for (std::size_t i = 0; i < count_; ++i) {
    if (!(reasons_[i] == other.reasons_[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace mbg
