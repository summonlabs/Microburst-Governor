// Microburst Governor - bounded corrective intent.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/intervention.hpp"

#include <string>

#include "mbg/limits.hpp"

namespace mbg {

const char* to_string(InterventionKind kind) noexcept {
  switch (kind) {
    case InterventionKind::kRequestPacing: return "request-pacing";
    case InterventionKind::kTemporaryAdmissionReduction: return "temporary-admission-reduction";
    case InterventionKind::kTemporaryHeadroomIncrease: return "temporary-headroom-increase";
    case InterventionKind::kEscalateToCongestionFabric: return "escalate-to-congestion-fabric";
  }
  return "unknown";
}

bool is_valid_intervention_kind(std::uint8_t raw) noexcept {
  return raw < static_cast<std::uint8_t>(kInterventionKindCount);
}

const char* to_string(InterventionState state) noexcept {
  switch (state) {
    case InterventionState::kRequested: return "requested";
    case InterventionState::kActive: return "active";
    case InterventionState::kExpired: return "expired";
    case InterventionState::kRevoked: return "revoked";
    case InterventionState::kFenced: return "fenced";
  }
  return "unknown";
}

const char* to_string(AuthorityVerdict verdict) noexcept {
  switch (verdict) {
    case AuthorityVerdict::kValid: return "valid";
    case AuthorityVerdict::kExpired: return "expired";
    case AuthorityVerdict::kPolicyChanged: return "policy-changed";
    case AuthorityVerdict::kEpochChanged: return "epoch-changed";
    case AuthorityVerdict::kIncarnationChanged: return "incarnation-changed";
    case AuthorityVerdict::kBootChanged: return "boot-changed";
    case AuthorityVerdict::kResourceGenerationChanged: return "resource-generation-changed";
    case AuthorityVerdict::kNotIssued: return "not-issued";
    case AuthorityVerdict::kEventClosed: return "event-closed";
    case AuthorityVerdict::kInterventionsDisabled: return "interventions-disabled";
    case AuthorityVerdict::kEpisodeFenced: return "episode-fenced";
    case AuthorityVerdict::kDurabilityLost: return "durability-lost";
  }
  return "unknown";
}

AuthorityVerdict evaluate_authority(const AuthorityVector& vector, const AuthorityContext& context,
                                    ResourceGeneration observed_resource_generation,
                                    Tick now) noexcept {
  // An intent that names no policy generation was never issued and holds no authority. The tick
  // zero value is a legitimate tick, so generation 0 is the explicit "not issued" marker.
  if (vector.policy_generation.raw() == 0) {
    return AuthorityVerdict::kNotIssued;
  }
  if (tick_after(now, vector.issued_tick)) {
    // Issued in the future relative to the observation point: contradictory, so not in force.
    return AuthorityVerdict::kNotIssued;
  }
  if (vector.boot.valid() && context.boot.valid() && vector.boot != context.boot) {
    return AuthorityVerdict::kBootChanged;
  }
  if (vector.incarnation.valid() && context.incarnation.valid() &&
      vector.incarnation != context.incarnation) {
    return AuthorityVerdict::kIncarnationChanged;
  }
  if (vector.epoch.valid() && context.epoch.valid() && vector.epoch != context.epoch) {
    return AuthorityVerdict::kEpochChanged;
  }
  if (vector.policy_generation != context.policy_generation ||
      vector.policy_digest != context.policy_digest) {
    return AuthorityVerdict::kPolicyChanged;
  }
  if (observed_resource_generation.valid() && vector.resource_generation.valid() &&
      observed_resource_generation != vector.resource_generation) {
    return AuthorityVerdict::kResourceGenerationChanged;
  }
  if (!tick_after(now, vector.expiry_tick)) {
    // now is at or past the expiry tick in wrapping-safe order.
    return AuthorityVerdict::kExpired;
  }
  return AuthorityVerdict::kValid;
}

void encode(const InterventionIntent& intent, ByteWriter w) {
  w.u64(intent.id.raw());
  w.u64(intent.event.raw());
  w.u64(intent.stream.resource.raw());
  w.u64(intent.stream.queue.raw());
  w.u64(intent.stream.path.valid() ? intent.stream.path.raw() : PathId::kInvalid);
  w.u8(static_cast<std::uint8_t>(intent.kind));
  w.u8(static_cast<std::uint8_t>(intent.state));
  w.u32(intent.magnitude_permille);
  w.u64(intent.ttl_ticks.value());
  w.u32(intent.authority.policy_generation.raw());
  w.u64(intent.authority.policy_digest);
  w.u64(intent.authority.evidence_generation.raw());
  w.u32(intent.authority.resource_generation.raw());
  w.u32(intent.authority.epoch.raw());
  w.u64(intent.authority.incarnation.raw());
  w.u64(intent.authority.boot.raw());
  w.u64(intent.authority.issued_tick.value());
  w.u64(intent.authority.expiry_tick.value());
  w.u64(intent.seq.raw());
  w.u8(static_cast<std::uint8_t>(intent.justification_severity));
  w.u16(static_cast<std::uint16_t>(intent.trigger));
  w.u64(intent.state_changed_tick.value());
  w.u8(static_cast<std::uint8_t>(intent.revocation_verdict));
}

Status decode(ByteReader& r, InterventionIntent& intent) {
  intent.id = InterventionId::from_raw(r.u64());
  intent.event = EventId::from_raw(r.u64());
  intent.stream.resource = ResourceId::from_raw(r.u64());
  intent.stream.queue = QueueId::from_raw(r.u64());
  intent.stream.path = PathId::from_raw(r.u64());
  intent.kind = static_cast<InterventionKind>(r.u8());
  intent.state = static_cast<InterventionState>(r.u8());
  intent.magnitude_permille = r.u32();
  intent.ttl_ticks = Tick::from_raw(r.u64());
  intent.authority.policy_generation = PolicyGeneration::from_raw(r.u32());
  intent.authority.policy_digest = r.u64();
  intent.authority.evidence_generation = EvidenceGeneration::from_raw(r.u64());
  intent.authority.resource_generation = ResourceGeneration::from_raw(r.u32());
  intent.authority.epoch = Epoch::from_raw(r.u32());
  intent.authority.incarnation = IncarnationId::from_raw(r.u64());
  intent.authority.boot = BootId::from_raw(r.u64());
  intent.authority.issued_tick = Tick::from_raw(r.u64());
  intent.authority.expiry_tick = Tick::from_raw(r.u64());
  intent.seq = DecisionSeq::from_raw(r.u64());
  intent.justification_severity = static_cast<SeverityClass>(r.u8());
  intent.trigger = static_cast<ReasonCode>(r.u16());
  intent.state_changed_tick = Tick::from_raw(r.u64());
  intent.revocation_verdict = static_cast<AuthorityVerdict>(r.u8());

  if (!r.ok()) {
    return Status::failure(StatusCode::kTruncated, "intervention payload is truncated");
  }
  if (!is_valid_intervention_kind(static_cast<std::uint8_t>(intent.kind))) {
    return Status::failure(StatusCode::kCorrupt, "intervention kind is out of range");
  }
  if (static_cast<std::uint8_t>(intent.state) >
      static_cast<std::uint8_t>(InterventionState::kFenced)) {
    return Status::failure(StatusCode::kCorrupt, "intervention state is out of range");
  }
  if (intent.magnitude_permille > limits::kMaxMagnitudePermille) {
    return Status::failure(StatusCode::kCorrupt, "intervention magnitude is out of range");
  }
  if (!is_valid_severity(static_cast<std::uint8_t>(intent.justification_severity))) {
    return Status::failure(StatusCode::kCorrupt, "intervention severity is out of range");
  }
  if (!intent.stream.valid()) {
    return Status::failure(StatusCode::kCorrupt, "intervention stream identity is incomplete");
  }
  return Status{};
}

std::string render_intervention_line(const InterventionIntent& intent) {
  std::string out;
  out.reserve(192);
  out.append("intervention=");
  out.append(std::to_string(intent.id.raw()));
  out.append(" event=");
  out.append(std::to_string(intent.event.raw()));
  out.append(" kind=");
  out.append(to_string(intent.kind));
  out.append(" state=");
  out.append(to_string(intent.state));
  out.append(" magnitude_permille=");
  out.append(std::to_string(intent.magnitude_permille));
  out.append(" ttl=");
  out.append(std::to_string(intent.ttl_ticks.value()));
  out.append(" policy_gen=");
  out.append(std::to_string(intent.authority.policy_generation.raw()));
  out.append(" epoch=");
  out.append(std::to_string(intent.authority.epoch.raw()));
  out.append(" verdict=");
  out.append(to_string(intent.revocation_verdict));
  return out;
}

}  // namespace mbg
