// Microburst Governor - identity domains.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "mbg/core/hash.hpp"
#include "mbg/core/strong_id.hpp"

namespace mbg {

// ---------------------------------------------------------------------------
// Topology identity (supplied by the surrounding fabric; never minted from telemetry)
// ---------------------------------------------------------------------------

/// A physical or logical resource that owns one or more queues (port, egress group, fabric link).
struct ResourceIdTag {
  static constexpr const char* kName = "ResourceId";
};
using ResourceId = StrongId<ResourceIdTag>;

/// A queue or buffer instance on a resource.
struct QueueIdTag {
  static constexpr const char* kName = "QueueId";
};
using QueueId = StrongId<QueueIdTag>;

/// A forwarding path that the burst traverses. Optional: the invalid value means "not reported".
struct PathIdTag {
  static constexpr const char* kName = "PathId";
};
using PathId = StrongId<PathIdTag>;

/// A traffic class / priority group affected by a burst. Optional.
struct ClassIdTag {
  static constexpr const char* kName = "ClassId";
};
using ClassId = StrongId<ClassIdTag>;

/// Generation of a resource structural configuration (capacity, shard layout). A change fences every
/// decision that was bound to the previous generation.
struct ResourceGenerationTag {
  static constexpr const char* kName = "ResourceGeneration";
};
using ResourceGeneration = StrongId<ResourceGenerationTag, std::uint32_t>;

// ---------------------------------------------------------------------------
// Evidence identity
// ---------------------------------------------------------------------------

/// Monotonic per-stream sample sequence number assigned by the producer.
struct SampleSeqTag {
  static constexpr const char* kName = "SampleSeq";
};
using SampleSeq = StrongId<SampleSeqTag>;

/// Identity of the provenance chain that produced a sample (collector incarnation + stream).
struct ProvenanceIdTag {
  static constexpr const char* kName = "ProvenanceId";
};
using ProvenanceId = StrongId<ProvenanceIdTag>;

/// Generation of the evidence set a decision was bound to.
struct EvidenceGenerationTag {
  static constexpr const char* kName = "EvidenceGeneration";
};
using EvidenceGeneration = StrongId<EvidenceGenerationTag>;

// ---------------------------------------------------------------------------
// Decision identity
// ---------------------------------------------------------------------------

/// Stable identity of a burst episode. Derived deterministically from the affected stream and the
/// onset tick so that replaying the same evidence yields the same identity.
struct EventIdTag {
  static constexpr const char* kName = "EventId";
};
using EventId = StrongId<EventIdTag>;

/// Identity of a bounded intervention intent.
struct InterventionIdTag {
  static constexpr const char* kName = "InterventionId";
};
using InterventionId = StrongId<InterventionIdTag>;

/// Identity of a policy document.
struct PolicyIdTag {
  static constexpr const char* kName = "PolicyId";
};
using PolicyId = StrongId<PolicyIdTag>;

/// Monotonic generation of an installed policy set.
struct PolicyGenerationTag {
  static constexpr const char* kName = "PolicyGeneration";
};
using PolicyGeneration = StrongId<PolicyGenerationTag, std::uint32_t>;

/// Monotonic sequence of an emitted decision record.
struct DecisionSeqTag {
  static constexpr const char* kName = "DecisionSeq";
};
using DecisionSeq = StrongId<DecisionSeqTag>;

// ---------------------------------------------------------------------------
// Authority identity
// ---------------------------------------------------------------------------

/// Coordinator authority epoch. Advanced on every coordinator restart.
struct EpochTag {
  static constexpr const char* kName = "Epoch";
};
using Epoch = StrongId<EpochTag, std::uint32_t>;

/// Identity of a running coordinator or worker process incarnation.
struct IncarnationIdTag {
  static constexpr const char* kName = "IncarnationId";
};
using IncarnationId = StrongId<IncarnationIdTag>;

/// Identity of a durable-state boot generation. Persisted, advanced on every restart.
struct BootIdTag {
  static constexpr const char* kName = "BootId";
};
using BootId = StrongId<BootIdTag>;

/// Identity of a registered worker.
struct WorkerIdTag {
  static constexpr const char* kName = "WorkerId";
};
using WorkerId = StrongId<WorkerIdTag>;

/// Identity of a single authority attempt (registration, renewal, fencing action).
struct AttemptIdTag {
  static constexpr const char* kName = "AttemptId";
};
using AttemptId = StrongId<AttemptIdTag>;

/// Sequence number of a framed wire message on a connection.
struct FrameSeqTag {
  static constexpr const char* kName = "FrameSeq";
};
using FrameSeq = StrongId<FrameSeqTag>;

/// Sequence number of a durable journal record.
struct RecordSeqTag {
  static constexpr const char* kName = "RecordSeq";
};
using RecordSeq = StrongId<RecordSeqTag>;

}  // namespace mbg
