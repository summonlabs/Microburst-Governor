// Microburst Governor - durable governor state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/model/event.hpp"
#include "mbg/model/intervention.hpp"
#include "mbg/model/policy.hpp"

namespace mbg::persist {

/// How a recovered artefact is classified by recovery. The distinction matters: durable
/// configuration and committed history are restored, while live authority and evidence freshness
/// never are.
enum class RecoveredCategory : std::uint8_t {
  kDurableConfiguration = 0,        ///< policy documents, limits, identities
  kCommittedHistory = 1,            ///< episodes that reached a terminal state before the restart
  kUnfinishedAttempt = 2,           ///< an episode or intervention that was mid flight
  kAmbiguousOutcome = 3,            ///< a record whose commit outcome cannot be established
  kStaleLiveAuthority = 4,          ///< authority that must not be restored as live
  kEvidenceRequiringRevalidation = 5,  ///< telemetry freshness must be re-established from scratch
};

[[nodiscard]] const char* to_string(RecoveredCategory category) noexcept;

/// Everything the governor persists. Live authority is deliberately absent: an intervention is
/// recorded so that its revocation can be proven, never so that it can be resumed.
struct PersistedState {
  BootId boot{};
  Epoch epoch{};
  IncarnationId incarnation{};

  PolicySet policies{};

  std::vector<BurstEvent> events{};
  std::vector<InterventionIntent> interventions{};

  DecisionSeq last_decision_seq{};
  RecordSeq last_record_seq{};
  Tick last_tick{};

  std::uint64_t decisions{0};
  std::uint64_t episodes_opened{0};
  std::uint64_t episodes_closed{0};
  std::uint64_t episodes_fenced{0};
  std::uint64_t interventions_requested{0};
  std::uint64_t interventions_revoked{0};
};

void encode(const PersistedState& state, std::vector<std::byte>& out);
[[nodiscard]] Status decode(std::span<const std::byte> bytes, PersistedState& state);

/// Result of recovery, expressed in the recovery vocabulary.
struct RestoreReport {
  bool snapshot_present{false};
  std::uint64_t records_applied{0};
  std::uint64_t configuration_items{0};
  std::uint64_t history_items{0};
  std::uint64_t unfinished_attempts{0};
  std::uint64_t ambiguous_outcomes{0};
  std::uint64_t stale_authority_items{0};
  std::uint64_t evidence_requiring_revalidation{0};
  std::string recovery_outcome{};
  std::string detail{};

  [[nodiscard]] std::uint64_t count(RecoveredCategory category) const noexcept;
};

}  // namespace mbg::persist
