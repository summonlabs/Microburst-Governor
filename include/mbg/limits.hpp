// Microburst Governor - hard resource bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace mbg {

/// Hard limits. Policy may select anything at or below these bounds but can never exceed them:
/// every externally influenced population, buffer, explanation and durable artefact is bounded
/// here so that malformed or hostile input cannot drive unbounded growth.
namespace limits {

// Evidence and detection
inline constexpr std::size_t kMaxWindowCapacity = 4096;
inline constexpr std::size_t kMinWindowCapacity = 2;
inline constexpr std::size_t kMaxStreams = 1024;
inline constexpr std::size_t kMaxProvenanceRefs = 8;
inline constexpr std::size_t kMaxReasons = 16;
inline constexpr std::uint64_t kMaxSustainTicks = 1ULL << 32;
inline constexpr std::uint64_t kMaxCooldownTicks = 1ULL << 32;
inline constexpr std::uint64_t kMaxEpisodeTicks = 1ULL << 40;
inline constexpr std::uint64_t kMaxWindowSpanTicks = 1ULL << 32;

// Events, interventions, policies
inline constexpr std::size_t kMaxEventHistory = 4096;
inline constexpr std::size_t kMaxRecoveredRecords = 65536;
inline constexpr std::size_t kMaxInterventions = 1024;
inline constexpr std::size_t kMaxPolicyOverrides = 256;
inline constexpr std::uint64_t kMaxInterventionTtlTicks = 1ULL << 32;
inline constexpr std::uint32_t kMaxMagnitudePermille = 1000;

// Emission queues
inline constexpr std::size_t kMaxPendingEmissions = 4096;

// Transport
inline constexpr std::uint32_t kMaxFramePayloadBytes = 1U << 20;   // 1 MiB
inline constexpr std::size_t kMaxConnections = 256;
inline constexpr std::uint32_t kMaxFramesPerConnectionPerBurst = 4096;

// Durability
inline constexpr std::uint64_t kMaxJournalBytes = 64ULL << 20;     // 64 MiB
inline constexpr std::uint64_t kMaxSnapshotBytes = 64ULL << 20;    // 64 MiB
inline constexpr std::uint32_t kMaxJournalRecordBytes = 1U << 20;  // 1 MiB
inline constexpr std::uint64_t kMaxEventsPersisted = 65536;

// Text / explanation rendering
inline constexpr std::size_t kMaxRenderedExplanationBytes = 8192;
inline constexpr std::size_t kMaxTraceLineBytes = 4096;

}  // namespace limits

}  // namespace mbg
