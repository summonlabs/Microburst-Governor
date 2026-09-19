// Microburst Governor - durable governor state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/persist/snapshot.hpp"

#include "mbg/core/bytes.hpp"
#include "mbg/core/checked.hpp"
#include "mbg/limits.hpp"
#include "mbg/version.hpp"

namespace mbg::persist {
namespace {

constexpr std::uint32_t kPersistedMagic = 0x4D424750U;  // "MBGP"

void encode_policy_set(const PolicySet& set, ByteWriter writer) {
  writer.u32(set.generation().raw());
  encode(set.fallback(), writer);
  writer.u32(static_cast<std::uint32_t>(set.overrides().size()));
  for (const PolicyOverride& entry : set.overrides()) {
    writer.u64(entry.resource.raw());
    encode(entry.policy, writer);
  }
}

Status decode_policy_set(ByteReader& reader, PolicySet& set) {
  const std::uint32_t generation = reader.u32();
  Policy fallback;
  Status status = decode(reader, fallback);
  if (!status.ok()) {
    return status;
  }
  const std::uint32_t override_count = reader.u32();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "policy set payload is truncated");
  }
  if (override_count > limits::kMaxPolicyOverrides) {
    return Status::failure(StatusCode::kOversized, "policy override count exceeds the bound");
  }
  std::vector<PolicyOverride> overrides;
  overrides.reserve(override_count);
  for (std::uint32_t i = 0; i < override_count; ++i) {
    PolicyOverride entry;
    entry.resource = ResourceId::from_raw(reader.u64());
    status = decode(reader, entry.policy);
    if (!status.ok()) {
      return status;
    }
    overrides.push_back(entry);
  }
  return PolicySet::restore(fallback, overrides, PolicyGeneration::from_raw(generation), set);
}

}  // namespace

const char* to_string(RecoveredCategory category) noexcept {
  switch (category) {
    case RecoveredCategory::kDurableConfiguration: return "durable-configuration";
    case RecoveredCategory::kCommittedHistory: return "committed-history";
    case RecoveredCategory::kUnfinishedAttempt: return "unfinished-attempt";
    case RecoveredCategory::kAmbiguousOutcome: return "ambiguous-outcome";
    case RecoveredCategory::kStaleLiveAuthority: return "stale-live-authority";
    case RecoveredCategory::kEvidenceRequiringRevalidation: return "evidence-requiring-revalidation";
  }
  return "unknown";
}

std::uint64_t RestoreReport::count(RecoveredCategory category) const noexcept {
  switch (category) {
    case RecoveredCategory::kDurableConfiguration: return configuration_items;
    case RecoveredCategory::kCommittedHistory: return history_items;
    case RecoveredCategory::kUnfinishedAttempt: return unfinished_attempts;
    case RecoveredCategory::kAmbiguousOutcome: return ambiguous_outcomes;
    case RecoveredCategory::kStaleLiveAuthority: return stale_authority_items;
    case RecoveredCategory::kEvidenceRequiringRevalidation: return evidence_requiring_revalidation;
  }
  return 0;
}

void encode(const PersistedState& state, std::vector<std::byte>& out) {
  out.clear();
  out.reserve(4096);
  ByteWriter writer(out);
  writer.u32(kPersistedMagic);
  writer.u16(kStateFormatVersion);
  writer.u16(0);
  writer.u64(state.boot.raw());
  writer.u32(state.epoch.raw());
  writer.u64(state.incarnation.raw());
  encode_policy_set(state.policies, writer);

  writer.u32(static_cast<std::uint32_t>(state.events.size()));
  for (const BurstEvent& event : state.events) {
    encode(event, writer);
  }
  writer.u32(static_cast<std::uint32_t>(state.interventions.size()));
  for (const InterventionIntent& intent : state.interventions) {
    encode(intent, writer);
  }
  writer.u64(state.last_decision_seq.raw());
  writer.u64(state.last_record_seq.raw());
  writer.u64(state.last_tick.value());
  writer.u64(state.decisions);
  writer.u64(state.episodes_opened);
  writer.u64(state.episodes_closed);
  writer.u64(state.episodes_fenced);
  writer.u64(state.interventions_requested);
  writer.u64(state.interventions_revoked);
}

Status decode(std::span<const std::byte> bytes, PersistedState& state) {
  if (bytes.size() > limits::kMaxSnapshotBytes) {
    return Status::failure(StatusCode::kOversized, "persisted state exceeds the bound");
  }
  ByteReader reader(bytes);
  if (reader.u32() != kPersistedMagic) {
    return Status::failure(StatusCode::kCorrupt, "persisted state magic mismatch");
  }
  if (reader.u16() != kStateFormatVersion) {
    return Status::failure(StatusCode::kUnsupported, "persisted state version is not supported");
  }
  (void)reader.u16();
  state.boot = BootId::from_raw(reader.u64());
  state.epoch = Epoch::from_raw(reader.u32());
  state.incarnation = IncarnationId::from_raw(reader.u64());
  Status status = decode_policy_set(reader, state.policies);
  if (!status.ok()) {
    return status;
  }

  const std::uint32_t event_count = reader.u32();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "persisted state is truncated");
  }
  if (event_count > limits::kMaxEventsPersisted) {
    return Status::failure(StatusCode::kOversized, "persisted event count exceeds the bound");
  }
  state.events.clear();
  state.events.reserve(event_count);
  for (std::uint32_t i = 0; i < event_count; ++i) {
    BurstEvent event;
    status = mbg::decode(reader, event);
    if (!status.ok()) {
      return status;
    }
    state.events.push_back(event);
  }

  const std::uint32_t intervention_count = reader.u32();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "persisted state is truncated");
  }
  if (intervention_count > limits::kMaxInterventions) {
    return Status::failure(StatusCode::kOversized, "persisted intervention count exceeds the bound");
  }
  state.interventions.clear();
  state.interventions.reserve(intervention_count);
  for (std::uint32_t i = 0; i < intervention_count; ++i) {
    InterventionIntent intent;
    status = mbg::decode(reader, intent);
    if (!status.ok()) {
      return status;
    }
    state.interventions.push_back(intent);
  }

  state.last_decision_seq = DecisionSeq::from_raw(reader.u64());
  state.last_record_seq = RecordSeq::from_raw(reader.u64());
  state.last_tick = Tick::from_raw(reader.u64());
  state.decisions = reader.u64();
  state.episodes_opened = reader.u64();
  state.episodes_closed = reader.u64();
  state.episodes_fenced = reader.u64();
  state.interventions_requested = reader.u64();
  state.interventions_revoked = reader.u64();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "persisted state is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "persisted state carries trailing bytes");
  }
  return Status{};
}

}  // namespace mbg::persist
