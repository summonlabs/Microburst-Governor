// Microburst Governor - detection and intervention policy.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mbg/core/bytes.hpp"
#include "mbg/core/status.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/intervention.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/severity.hpp"

namespace mbg {

/// Ordered ascending grade ladder.
///
/// Four steps map to minor, moderate, severe and critical. A step equal to the maximum representable
/// value is disabled (it can never be reached), which is how a deployment turns a dimension off
/// without special casing it. Ladders must be non-decreasing; validation enforces that.
struct GradeLadder {
  static constexpr std::size_t kSteps = 4;
  std::array<std::uint64_t, kSteps> thresholds{};

  [[nodiscard]] SeverityClass grade(std::uint64_t value) const noexcept;
  [[nodiscard]] SeverityClass grade_rate(RateQ16 value) const noexcept;
  [[nodiscard]] bool monotonic() const noexcept;

  friend constexpr bool operator==(const GradeLadder&, const GradeLadder&) noexcept = default;
};

/// Deterministic detection rules. Every threshold is explicit: there is no implicit tuning, no
/// adaptive learning and no dependence on wall clock.
struct DetectionPolicy {
  // --- evidence admissibility
  std::uint64_t window_capacity{64};
  std::uint64_t nominal_sample_interval_ticks{1};
  std::uint64_t max_sample_gap_ticks{4};
  std::uint64_t min_samples_for_classification{8};
  std::uint32_t min_completeness_permille{900};
  std::uint64_t staleness_limit_ticks{16};
  std::uint64_t reorder_tolerance_ticks{0};

  // --- onset rules
  std::uint64_t onset_min_depth{1024};
  RateQ16 onset_min_slope_q16{rate_from_whole(8)};
  std::uint64_t onset_sustain_ticks{2};

  /// How long after the depth threshold is first crossed the rise-rate condition may still be
  /// satisfied. This is what makes the rule a *transient* rule: a queue that merely sits above the
  /// threshold and later drifts upward is not a microburst, because the rise has to happen inside
  /// the onset window.
  std::uint64_t onset_slope_window_ticks{4};

  // --- release rules (hysteresis: strictly below onset)
  std::uint64_t release_depth{256};
  RateQ16 release_slope_q16{rate_from_whole(2)};
  std::uint64_t release_sustain_ticks{3};

  // --- severity ladders
  GradeLadder occupancy_permille_ladder{{700, 850, 950, 990}};
  GradeLadder depth_ladder{{512, 2048, 8192, 32768}};
  GradeLadder slope_ladder{
      {rate_from_whole(4), rate_from_whole(16), rate_from_whole(64), rate_from_whole(256)}};
  GradeLadder duration_ladder{{2, 8, 32, 128}};
  GradeLadder drop_ladder{{1, 16, 256, 4096}};

  // --- episode shaping
  std::uint64_t onset_merge_ticks{8};
  std::uint64_t cooldown_ticks{32};
  std::uint64_t max_episode_ticks{4096};

  /// How long an already-open episode survives inadmissible evidence before it is fenced. Detection
  /// itself is never positive while evidence is inadmissible; this only governs how long an existing
  /// episode is suspended rather than closed.
  std::uint64_t evidence_grace_ticks{8};

  // --- degraded operation
  bool allow_degraded_classification{false};
  std::uint32_t degraded_min_completeness_permille{500};
  SeverityClass degraded_severity_cap{SeverityClass::kMinor};

  // --- generation semantics
  bool allow_capacity_change_within_window{false};

  friend constexpr bool operator==(const DetectionPolicy&, const DetectionPolicy&) noexcept = default;
};

/// Per-kind bounds for corrective intent. A kind that is not enabled is never requested.
struct InterventionKindLimit {
  bool enabled{false};
  std::uint32_t magnitude_permille{0};
  std::uint64_t ttl_ticks{0};
  SeverityClass min_severity{SeverityClass::kModerate};

  friend constexpr bool operator==(const InterventionKindLimit&,
                                   const InterventionKindLimit&) noexcept = default;
};

struct InterventionPolicy {
  std::array<InterventionKindLimit, kInterventionKindCount> kinds{};
  std::uint32_t max_concurrent_per_stream{2};
  std::uint32_t max_concurrent_total{64};
  bool require_authoritative_evidence{true};

  friend constexpr bool operator==(const InterventionPolicy&, const InterventionPolicy&) noexcept = default;
};

/// A named, versioned policy document.
struct Policy {
  PolicyId id{};
  DetectionPolicy detection{};
  InterventionPolicy intervention{};

  /// Canonical digest of the semantic content. Two policies with the same digest classify
  /// identically; a digest change invalidates every decision bound to the previous one.
  [[nodiscard]] std::uint64_t digest() const noexcept;

  friend bool operator==(const Policy&, const Policy&) noexcept = default;
};

/// Mints a stable policy identity from a name so that restarts resolve to the same identity.
[[nodiscard]] PolicyId make_policy_id(std::string_view name) noexcept;

/// The conservative default policy. It performs no intervention: every intervention kind is
/// disabled until a deployment enables it explicitly.
[[nodiscard]] Policy make_default_policy() noexcept;

/// A per-resource policy override, ordered by resource identity for deterministic persistence.
struct PolicyOverride {
  ResourceId resource{};
  Policy policy{};

  friend bool operator==(const PolicyOverride&, const PolicyOverride&) noexcept = default;
};

/// The installed policy set: one fallback policy plus a bounded set of per-resource overrides.
///
/// The generation advances on every accepted mutation, and the set digest folds the generation in,
/// so any change to detection or intervention behaviour is observable to every decision that
/// carries a policy digest.
class PolicySet {
 public:
  PolicySet();

  [[nodiscard]] static Status validate(const Policy& policy) noexcept;

  Status set_fallback(Policy policy);
  Status set_override(ResourceId resource, Policy policy);
  Status remove_override(ResourceId resource);
  void clear_overrides();

  [[nodiscard]] const Policy& resolve(ResourceId resource) const noexcept;
  [[nodiscard]] const Policy& fallback() const noexcept { return fallback_; }
  [[nodiscard]] PolicyGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
  [[nodiscard]] std::size_t override_count() const noexcept { return overrides_.size(); }
  [[nodiscard]] bool has_override(ResourceId resource) const noexcept;
  [[nodiscard]] const std::vector<PolicyOverride>& overrides() const noexcept { return overrides_; }

  /// Installs a fully specified set (used by recovery). Validates every policy and recomputes the
  /// digest; the generation is taken from the caller so that a restored generation is authoritative.
  static Status restore(const Policy& fallback, const std::vector<PolicyOverride>& overrides,
                        PolicyGeneration generation, PolicySet& out);

 private:
  void recompute_digest() noexcept;
  [[nodiscard]] Status bump_generation() noexcept;

  Policy fallback_{};
  std::vector<PolicyOverride> overrides_{};
  std::unordered_map<ResourceId, std::size_t> index_{};
  PolicyGeneration generation_{PolicyGeneration::from_raw(1)};
  std::uint64_t digest_{0};
};

void encode(const Policy& policy, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, Policy& policy);

}  // namespace mbg
