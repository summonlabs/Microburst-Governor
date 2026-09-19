// Microburst Governor - bounded corrective intent.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "mbg/core/bytes.hpp"
#include "mbg/core/status.hpp"
#include "mbg/model/explanation.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/severity.hpp"
#include "mbg/model/tick.hpp"

namespace mbg {

/// The corrective intents the governor is allowed to express.
///
/// These are requests addressed to the systems that own the adjacent mechanisms. The governor never
/// performs them: it does not pace, does not drop, does not resize and does not schedule.
enum class InterventionKind : std::uint8_t {
  kRequestPacing = 0,
  kTemporaryAdmissionReduction = 1,
  kTemporaryHeadroomIncrease = 2,
  kEscalateToCongestionFabric = 3,
};

inline constexpr std::size_t kInterventionKindCount = 4;

[[nodiscard]] const char* to_string(InterventionKind kind) noexcept;
[[nodiscard]] bool is_valid_intervention_kind(std::uint8_t raw) noexcept;

/// Lifecycle of an intervention intent. A revoked or expired intent is never silently resurrected.
enum class InterventionState : std::uint8_t {
  kRequested = 0,
  kActive = 1,
  kExpired = 2,
  kRevoked = 3,
  kFenced = 4,
};

[[nodiscard]] const char* to_string(InterventionState state) noexcept;

/// The authority context that must still hold for an intervention to remain in force.
///
/// Every field is a generation or an incarnation. A change to any of them invalidates the intent,
/// which is what prevents an intervention from silently surviving a stale authority.
struct AuthorityVector {
  PolicyGeneration policy_generation{};
  std::uint64_t policy_digest{0};
  EvidenceGeneration evidence_generation{};
  ResourceGeneration resource_generation{};
  Epoch epoch{};
  IncarnationId incarnation{};
  BootId boot{};
  Tick issued_tick{};
  Tick expiry_tick{};

  friend constexpr bool operator==(const AuthorityVector&, const AuthorityVector&) noexcept = default;
};

/// The authority context currently observed by the governor.
struct AuthorityContext {
  PolicyGeneration policy_generation{};
  std::uint64_t policy_digest{0};
  Epoch epoch{};
  IncarnationId incarnation{};
  BootId boot{};

  friend constexpr bool operator==(const AuthorityContext&, const AuthorityContext&) noexcept = default;
};

/// Why an authority vector stopped being valid.
enum class AuthorityVerdict : std::uint8_t {
  kValid = 0,
  kExpired = 1,
  kPolicyChanged = 2,
  kEpochChanged = 3,
  kIncarnationChanged = 4,
  kBootChanged = 5,
  kResourceGenerationChanged = 6,
  kNotIssued = 7,
  kEventClosed = 8,
  kInterventionsDisabled = 9,
  kEpisodeFenced = 10,
  kDurabilityLost = 11,
};

[[nodiscard]] const char* to_string(AuthorityVerdict verdict) noexcept;

/// Evaluates whether an authority vector is still in force at the supplied tick.
[[nodiscard]] AuthorityVerdict evaluate_authority(const AuthorityVector& vector,
                                                  const AuthorityContext& context,
                                                  ResourceGeneration observed_resource_generation,
                                                  Tick now) noexcept;

struct InterventionIntent {
  InterventionId id{};
  EventId event{};
  StreamKey stream{};
  InterventionKind kind{InterventionKind::kRequestPacing};
  InterventionState state{InterventionState::kRequested};

  /// Bounded magnitude in per-mille of the requesting mechanism's full scale. Never above
  /// limits::kMaxMagnitudePermille and never above the policy limit for the kind.
  std::uint32_t magnitude_permille{0};

  Tick ttl_ticks{};
  AuthorityVector authority{};
  DecisionSeq seq{};
  SeverityClass justification_severity{SeverityClass::kNone};
  ReasonCode trigger{ReasonCode::kNone};
  Tick state_changed_tick{};
  AuthorityVerdict revocation_verdict{AuthorityVerdict::kValid};

  [[nodiscard]] bool live() const noexcept {
    return state == InterventionState::kRequested || state == InterventionState::kActive;
  }
};

void encode(const InterventionIntent& intent, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, InterventionIntent& intent);

[[nodiscard]] std::string render_intervention_line(const InterventionIntent& intent);

}  // namespace mbg
