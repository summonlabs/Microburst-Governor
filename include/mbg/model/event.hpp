// Microburst Governor - burst episode model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "mbg/core/bytes.hpp"
#include "mbg/core/status.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/explanation.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/severity.hpp"
#include "mbg/model/tick.hpp"

namespace mbg {

/// Lifecycle of an episode. A burst episode is opened, may be updated while evidence is
/// authoritative, and is closed (or fenced) exactly once.
enum class EventLifecycle : std::uint8_t {
  kOpen = 0,
  kRecovering = 1,
  kClosed = 2,
  kFenced = 3,
};

[[nodiscard]] const char* to_string(EventLifecycle lifecycle) noexcept;

/// Why an episode stopped being authoritative.
enum class CloseReason : std::uint8_t {
  kNone = 0,
  kReleaseConfirmed = 1,
  kEpisodeTimeout = 2,
  kEvidenceStale = 3,
  kPolicyChanged = 4,
  kAuthorityLost = 5,
  kRestart = 6,
  kCapacityChanged = 7,
  kReplayEnd = 8,
};

[[nodiscard]] const char* to_string(CloseReason reason) noexcept;

/// Completeness and quality of the evidence window that justified a decision.
struct EvidenceSummary {
  Tick window_start{};
  Tick window_end{};
  std::uint64_t samples{0};
  std::uint64_t expected_samples{0};
  std::uint32_t completeness_permille{0};
  std::uint64_t gaps{0};
  std::uint64_t reordered{0};
  std::uint64_t duplicates{0};
  std::uint64_t counter_resets{0};
  std::uint64_t counter_rollovers{0};
  std::uint64_t dropped_samples{0};
  bool capacity_known{false};
  std::uint64_t capacity_bytes{0};
  bool sparse{false};
  bool synthetic{false};
  ProvenanceId provenance{};
  std::uint32_t provenance_count{0};

  friend constexpr bool operator==(const EvidenceSummary&, const EvidenceSummary&) noexcept = default;
};

/// Peak and aggregate quantities of a burst episode.
struct BurstMetrics {
  std::uint64_t peak_depth{0};
  std::uint64_t peak_occupancy_bytes{0};
  std::uint64_t capacity_bytes{0};
  RateQ16 peak_slope_q16{0};
  RateQ16 peak_imbalance_q16{0};
  Tick onset_tick{};
  Tick peak_tick{};
  Tick last_update_tick{};
  Tick end_tick{};
  std::uint64_t duration_ticks{0};
  std::uint64_t drop_delta{0};
  std::uint64_t mark_delta{0};

  friend constexpr bool operator==(const BurstMetrics&, const BurstMetrics&) noexcept = default;
};

/// A burst episode. The identity is derived deterministically from the stream and onset tick so a
/// replay of the same evidence produces the same episode identity.
struct BurstEvent {
  EventId id{};
  StreamKey stream{};
  ResourceGeneration resource_generation{};
  EventLifecycle lifecycle{EventLifecycle::kOpen};
  SeverityClass severity{SeverityClass::kNone};
  EvidenceAuthority authority{EvidenceAuthority::kUnknown};

  BurstMetrics metrics{};
  EvidenceSummary evidence{};

  PolicyId policy_id{};
  PolicyGeneration policy_generation{};
  std::uint64_t policy_digest{0};

  Epoch epoch{};
  IncarnationId incarnation{};
  BootId boot{};

  CloseReason close_reason{CloseReason::kNone};
  Tick closed_tick{};
  DecisionSeq opened_seq{};
  DecisionSeq last_seq{};
  std::uint32_t revision{0};

  [[nodiscard]] bool open() const noexcept {
    return lifecycle == EventLifecycle::kOpen || lifecycle == EventLifecycle::kRecovering;
  }
  [[nodiscard]] bool positive() const noexcept { return severity != SeverityClass::kNone; }
};

/// Deterministic episode identity. Two runs over identical evidence produce identical identities,
/// and the identity does not depend on wall clock, allocation order or hashing of pointers.
[[nodiscard]] EventId make_event_id(const StreamKey& stream, Tick onset_tick) noexcept;

void encode(const BurstEvent& event, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, BurstEvent& event);

/// Renders a single line summary, bounded by limits::kMaxRenderedExplanationBytes.
[[nodiscard]] std::string render_event_line(const BurstEvent& event);

}  // namespace mbg
