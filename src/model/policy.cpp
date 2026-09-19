// Microburst Governor - detection and intervention policy.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/model/policy.hpp"

#include <algorithm>

#include "mbg/core/checked.hpp"
#include "mbg/core/hash.hpp"

namespace mbg {
namespace {

constexpr std::uint64_t kDisabledStep = ~std::uint64_t{0};

void encode_ladder(const GradeLadder& ladder, ByteWriter w) {
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    w.u64(ladder.thresholds[i]);
  }
}

bool decode_ladder(ByteReader& r, GradeLadder& ladder) {
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    ladder.thresholds[i] = r.u64();
  }
  return r.ok();
}

void encode_kind_limit(const InterventionKindLimit& limit, ByteWriter w) {
  w.u8(limit.enabled ? 1U : 0U);
  w.u32(limit.magnitude_permille);
  w.u64(limit.ttl_ticks);
  w.u8(static_cast<std::uint8_t>(limit.min_severity));
}

bool decode_kind_limit(ByteReader& r, InterventionKindLimit& limit) {
  limit.enabled = r.u8() != 0;
  limit.magnitude_permille = r.u32();
  limit.ttl_ticks = r.u64();
  limit.min_severity = static_cast<SeverityClass>(r.u8());
  return r.ok();
}

}  // namespace

SeverityClass GradeLadder::grade(std::uint64_t value) const noexcept {
  SeverityClass result = SeverityClass::kNone;
  for (std::size_t i = 0; i < kSteps; ++i) {
    if (thresholds[i] == kDisabledStep) {
      continue;
    }
    if (value >= thresholds[i]) {
      result = static_cast<SeverityClass>(static_cast<std::uint8_t>(i) + 1U);
    }
  }
  return result;
}

SeverityClass GradeLadder::grade_rate(RateQ16 value) const noexcept {
  if (value <= 0) {
    return SeverityClass::kNone;
  }
  return grade(static_cast<std::uint64_t>(value));
}

bool GradeLadder::monotonic() const noexcept {
  for (std::size_t i = 1; i < kSteps; ++i) {
    if (thresholds[i] < thresholds[i - 1]) {
      // A disabled step sorts as the largest value; anything after it must also be disabled.
      return false;
    }
  }
  return true;
}

std::uint64_t Policy::digest() const noexcept {
  std::uint64_t h = 0x5143F00D1234ABCDULL;
  h = hash_combine(h, id.raw());
  h = hash_combine(h, detection.window_capacity);
  h = hash_combine(h, detection.nominal_sample_interval_ticks);
  h = hash_combine(h, detection.max_sample_gap_ticks);
  h = hash_combine(h, detection.min_samples_for_classification);
  h = hash_combine(h, detection.min_completeness_permille);
  h = hash_combine(h, detection.staleness_limit_ticks);
  h = hash_combine(h, detection.reorder_tolerance_ticks);
  h = hash_combine(h, detection.onset_min_depth);
  h = hash_combine(h, static_cast<std::uint64_t>(detection.onset_min_slope_q16));
  h = hash_combine(h, detection.onset_sustain_ticks);
  h = hash_combine(h, detection.onset_slope_window_ticks);
  h = hash_combine(h, detection.release_depth);
  h = hash_combine(h, static_cast<std::uint64_t>(detection.release_slope_q16));
  h = hash_combine(h, detection.release_sustain_ticks);
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    h = hash_combine(h, detection.occupancy_permille_ladder.thresholds[i]);
  }
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    h = hash_combine(h, detection.depth_ladder.thresholds[i]);
  }
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    h = hash_combine(h, detection.slope_ladder.thresholds[i]);
  }
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    h = hash_combine(h, detection.duration_ladder.thresholds[i]);
  }
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    h = hash_combine(h, detection.drop_ladder.thresholds[i]);
  }
  h = hash_combine(h, detection.onset_merge_ticks);
  h = hash_combine(h, detection.cooldown_ticks);
  h = hash_combine(h, detection.max_episode_ticks);
  h = hash_combine(h, detection.evidence_grace_ticks);
  h = hash_combine(h, detection.allow_degraded_classification ? 1U : 0U);
  h = hash_combine(h, detection.degraded_min_completeness_permille);
  h = hash_combine(h, static_cast<std::uint64_t>(detection.degraded_severity_cap));
  h = hash_combine(h, detection.allow_capacity_change_within_window ? 1U : 0U);
  for (std::size_t i = 0; i < kInterventionKindCount; ++i) {
    h = hash_combine(h, intervention.kinds[i].enabled ? 1U : 0U);
    h = hash_combine(h, intervention.kinds[i].magnitude_permille);
    h = hash_combine(h, intervention.kinds[i].ttl_ticks);
    h = hash_combine(h, static_cast<std::uint64_t>(intervention.kinds[i].min_severity));
  }
  h = hash_combine(h, intervention.max_concurrent_per_stream);
  h = hash_combine(h, intervention.max_concurrent_total);
  h = hash_combine(h, intervention.require_authoritative_evidence ? 1U : 0U);
  return h;
}

PolicyId make_policy_id(std::string_view name) noexcept {
  return PolicyId::from_raw(fnv1a64(name));
}

Policy make_default_policy() noexcept {
  Policy policy{};
  policy.id = make_policy_id("mbg.default.v1");
  policy.intervention.kinds[static_cast<std::size_t>(InterventionKind::kRequestPacing)] =
      InterventionKindLimit{true, 250, 64, SeverityClass::kModerate};
  policy.intervention
      .kinds[static_cast<std::size_t>(InterventionKind::kTemporaryAdmissionReduction)] =
      InterventionKindLimit{true, 125, 32, SeverityClass::kSevere};
  policy.intervention
      .kinds[static_cast<std::size_t>(InterventionKind::kTemporaryHeadroomIncrease)] =
      InterventionKindLimit{false, 0, 0, SeverityClass::kSevere};
  policy.intervention
      .kinds[static_cast<std::size_t>(InterventionKind::kEscalateToCongestionFabric)] =
      InterventionKindLimit{true, 1000, 128, SeverityClass::kSevere};
  return policy;
}

PolicySet::PolicySet() : fallback_(make_default_policy()) { recompute_digest(); }

Status PolicySet::validate(const Policy& policy) noexcept {
  const DetectionPolicy& d = policy.detection;

  if (d.window_capacity < limits::kMinWindowCapacity || d.window_capacity > limits::kMaxWindowCapacity) {
    return Status::failure(StatusCode::kOutOfRange, "window capacity outside supported bounds");
  }
  if (d.nominal_sample_interval_ticks == 0) {
    return Status::failure(StatusCode::kInvalidArgument, "nominal sample interval must be positive");
  }
  if (d.min_samples_for_classification < 2) {
    return Status::failure(StatusCode::kInvalidArgument, "at least two samples are required");
  }
  if (d.min_samples_for_classification > d.window_capacity) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "minimum samples exceed the window capacity");
  }
  if (d.min_completeness_permille > 1000) {
    return Status::failure(StatusCode::kOutOfRange, "completeness threshold above 1000 per-mille");
  }
  if (d.max_sample_gap_ticks < d.nominal_sample_interval_ticks) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "max sample gap below the nominal sample interval");
  }
  if (d.staleness_limit_ticks < d.nominal_sample_interval_ticks) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "staleness limit below the nominal sample interval");
  }
  if (d.staleness_limit_ticks > limits::kMaxWindowSpanTicks) {
    return Status::failure(StatusCode::kOutOfRange, "staleness limit above the supported span");
  }
  if (d.onset_sustain_ticks == 0 || d.onset_sustain_ticks > limits::kMaxSustainTicks) {
    return Status::failure(StatusCode::kOutOfRange, "onset sustain ticks outside supported bounds");
  }
  if (d.release_sustain_ticks == 0 || d.release_sustain_ticks > limits::kMaxSustainTicks) {
    return Status::failure(StatusCode::kOutOfRange, "release sustain ticks outside supported bounds");
  }
  if (d.onset_slope_window_ticks < d.onset_sustain_ticks ||
      d.onset_slope_window_ticks > limits::kMaxWindowSpanTicks) {
    return Status::failure(StatusCode::kOutOfRange,
                           "onset slope window must cover the sustain interval and stay bounded");
  }
  if (d.onset_min_depth == 0) {
    return Status::failure(StatusCode::kInvalidArgument, "onset depth threshold must be positive");
  }
  if (d.release_depth >= d.onset_min_depth) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "release depth must be strictly below onset depth (hysteresis)");
  }
  if (d.onset_min_slope_q16 < 0 || d.release_slope_q16 < 0) {
    return Status::failure(StatusCode::kInvalidArgument, "slope thresholds cannot be negative");
  }
  if (d.release_slope_q16 > d.onset_min_slope_q16) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "release slope must not exceed the onset slope (hysteresis)");
  }
  const GradeLadder* ladders[] = {&d.occupancy_permille_ladder, &d.depth_ladder, &d.slope_ladder,
                                  &d.duration_ladder, &d.drop_ladder};
  for (const GradeLadder* ladder : ladders) {
    if (!ladder->monotonic()) {
      return Status::failure(StatusCode::kInvalidArgument, "severity ladder is not monotonic");
    }
  }
  for (std::size_t i = 0; i < GradeLadder::kSteps; ++i) {
    if (d.occupancy_permille_ladder.thresholds[i] > 1000) {
      return Status::failure(StatusCode::kOutOfRange,
                             "occupancy ladder threshold above 1000 per-mille");
    }
  }
  if (d.onset_merge_ticks > limits::kMaxWindowSpanTicks) {
    return Status::failure(StatusCode::kOutOfRange, "onset merge window above the supported span");
  }
  if (d.cooldown_ticks > limits::kMaxCooldownTicks) {
    return Status::failure(StatusCode::kOutOfRange, "cooldown above the supported span");
  }
  if (d.max_episode_ticks == 0 || d.max_episode_ticks > limits::kMaxEpisodeTicks) {
    return Status::failure(StatusCode::kOutOfRange, "episode ceiling outside supported bounds");
  }
  if (d.evidence_grace_ticks == 0 || d.evidence_grace_ticks > limits::kMaxWindowSpanTicks) {
    return Status::failure(StatusCode::kOutOfRange, "evidence grace outside supported bounds");
  }
  if (d.allow_degraded_classification) {
    if (d.degraded_min_completeness_permille > d.min_completeness_permille) {
      return Status::failure(StatusCode::kInvalidArgument,
                             "degraded completeness floor above the authoritative floor");
    }
    if (severity_at_least(d.degraded_severity_cap, SeverityClass::kSevere)) {
      return Status::failure(StatusCode::kInvalidArgument,
                             "degraded severity cap must stay below severe");
    }
  }

  const InterventionPolicy& ip = policy.intervention;
  for (std::size_t i = 0; i < kInterventionKindCount; ++i) {
    const InterventionKindLimit& limit = ip.kinds[i];
    if (!limit.enabled) {
      continue;
    }
    if (limit.magnitude_permille == 0 || limit.magnitude_permille > limits::kMaxMagnitudePermille) {
      return Status::failure(StatusCode::kOutOfRange, "intervention magnitude out of range");
    }
    if (limit.ttl_ticks == 0 || limit.ttl_ticks > limits::kMaxInterventionTtlTicks) {
      return Status::failure(StatusCode::kOutOfRange, "intervention ttl out of range");
    }
    if (static_cast<std::uint8_t>(limit.min_severity) <
            static_cast<std::uint8_t>(SeverityClass::kMinor) ||
        !is_valid_severity(static_cast<std::uint8_t>(limit.min_severity))) {
      return Status::failure(StatusCode::kOutOfRange, "intervention severity floor out of range");
    }
  }
  if (ip.max_concurrent_per_stream == 0 || ip.max_concurrent_total == 0) {
    return Status::failure(StatusCode::kInvalidArgument, "concurrency limits must be positive");
  }
  if (ip.max_concurrent_per_stream > limits::kMaxInterventions ||
      ip.max_concurrent_total > limits::kMaxInterventions) {
    return Status::failure(StatusCode::kOutOfRange, "concurrency limits above supported bounds");
  }
  if (ip.max_concurrent_per_stream > ip.max_concurrent_total) {
    return Status::failure(StatusCode::kInvalidArgument,
                           "per-stream concurrency limit above the global limit");
  }
  return Status{};
}

Status PolicySet::bump_generation() noexcept {
  const auto next = add_checked<std::uint32_t>(generation_.raw(), 1U);
  if (!next.has_value()) {
    return Status::failure(StatusCode::kOverflow, "policy generation exhausted");
  }
  generation_ = PolicyGeneration::from_raw(*next);
  return Status{};
}

void PolicySet::recompute_digest() noexcept {
  std::uint64_t h = 0xB16B00B5DEADC0DEULL;
  h = hash_combine(h, generation_.raw());
  h = hash_combine(h, fallback_.digest());
  for (const PolicyOverride& entry : overrides_) {
    h = hash_combine(h, entry.resource.raw());
    h = hash_combine(h, entry.policy.digest());
  }
  digest_ = h;
}

Status PolicySet::set_fallback(Policy policy) {
  const Status status = validate(policy);
  if (!status.ok()) {
    return status;
  }
  const Status bump = bump_generation();
  if (!bump.ok()) {
    return bump;
  }
  fallback_ = policy;
  recompute_digest();
  return Status{};
}

bool PolicySet::has_override(ResourceId resource) const noexcept {
  return index_.find(resource) != index_.end();
}

Status PolicySet::set_override(ResourceId resource, Policy policy) {
  if (!resource.valid()) {
    return Status::failure(StatusCode::kInvalidArgument, "override resource identity is invalid");
  }
  const Status status = validate(policy);
  if (!status.ok()) {
    return status;
  }
  const auto existing = index_.find(resource);
  if (existing == index_.end() && overrides_.size() >= limits::kMaxPolicyOverrides) {
    return Status::failure(StatusCode::kCapacityExceeded, "policy override capacity exhausted");
  }
  const Status bump = bump_generation();
  if (!bump.ok()) {
    return bump;
  }
  if (existing != index_.end()) {
    overrides_[existing->second].policy = policy;
  } else {
    index_.emplace(resource, overrides_.size());
    overrides_.push_back(PolicyOverride{resource, policy});
  }
  recompute_digest();
  return Status{};
}

Status PolicySet::remove_override(ResourceId resource) {
  const auto existing = index_.find(resource);
  if (existing == index_.end()) {
    return Status::failure(StatusCode::kNotFound, "no override for the supplied resource");
  }
  const Status bump = bump_generation();
  if (!bump.ok()) {
    return bump;
  }
  overrides_.erase(overrides_.begin() + static_cast<std::ptrdiff_t>(existing->second));
  index_.clear();
  for (std::size_t i = 0; i < overrides_.size(); ++i) {
    index_.emplace(overrides_[i].resource, i);
  }
  recompute_digest();
  return Status{};
}

void PolicySet::clear_overrides() {
  if (overrides_.empty()) {
    return;
  }
  if (bump_generation().ok()) {
    overrides_.clear();
    index_.clear();
    recompute_digest();
  }
}

const Policy& PolicySet::resolve(ResourceId resource) const noexcept {
  const auto found = index_.find(resource);
  if (found == index_.end()) {
    return fallback_;
  }
  return overrides_[found->second].policy;
}

Status PolicySet::restore(const Policy& fallback, const std::vector<PolicyOverride>& overrides,
                          PolicyGeneration generation, PolicySet& out) {
  if (generation.raw() == 0) {
    return Status::failure(StatusCode::kInvalidArgument, "restored policy generation is zero");
  }
  if (overrides.size() > limits::kMaxPolicyOverrides) {
    return Status::failure(StatusCode::kCapacityExceeded, "restored override count above bounds");
  }
  Status status = validate(fallback);
  if (!status.ok()) {
    return status;
  }
  PolicySet candidate;
  candidate.overrides_.clear();
  candidate.index_.clear();
  for (const PolicyOverride& entry : overrides) {
    if (!entry.resource.valid()) {
      return Status::failure(StatusCode::kCorrupt, "restored override resource is invalid");
    }
    if (candidate.index_.find(entry.resource) != candidate.index_.end()) {
      return Status::failure(StatusCode::kCorrupt, "restored override set contains a duplicate");
    }
    status = validate(entry.policy);
    if (!status.ok()) {
      return status;
    }
    candidate.index_.emplace(entry.resource, candidate.overrides_.size());
    candidate.overrides_.push_back(entry);
  }
  candidate.fallback_ = fallback;
  candidate.generation_ = generation;
  candidate.recompute_digest();
  out = candidate;
  return Status{};
}

void encode(const Policy& policy, ByteWriter w) {
  w.u64(policy.id.raw());
  const DetectionPolicy& d = policy.detection;
  w.u64(d.window_capacity);
  w.u64(d.nominal_sample_interval_ticks);
  w.u64(d.max_sample_gap_ticks);
  w.u64(d.min_samples_for_classification);
  w.u32(d.min_completeness_permille);
  w.u64(d.staleness_limit_ticks);
  w.u64(d.reorder_tolerance_ticks);
  w.u64(d.onset_min_depth);
  w.i64(d.onset_min_slope_q16);
  w.u64(d.onset_sustain_ticks);
  w.u64(d.onset_slope_window_ticks);
  w.u64(d.release_depth);
  w.i64(d.release_slope_q16);
  w.u64(d.release_sustain_ticks);
  encode_ladder(d.occupancy_permille_ladder, w);
  encode_ladder(d.depth_ladder, w);
  encode_ladder(d.slope_ladder, w);
  encode_ladder(d.duration_ladder, w);
  encode_ladder(d.drop_ladder, w);
  w.u64(d.onset_merge_ticks);
  w.u64(d.cooldown_ticks);
  w.u64(d.max_episode_ticks);
  w.u64(d.evidence_grace_ticks);
  w.u8(d.allow_degraded_classification ? 1U : 0U);
  w.u32(d.degraded_min_completeness_permille);
  w.u8(static_cast<std::uint8_t>(d.degraded_severity_cap));
  w.u8(d.allow_capacity_change_within_window ? 1U : 0U);
  for (std::size_t i = 0; i < kInterventionKindCount; ++i) {
    encode_kind_limit(policy.intervention.kinds[i], w);
  }
  w.u32(policy.intervention.max_concurrent_per_stream);
  w.u32(policy.intervention.max_concurrent_total);
  w.u8(policy.intervention.require_authoritative_evidence ? 1U : 0U);
}

Status decode(ByteReader& r, Policy& policy) {
  policy.id = PolicyId::from_raw(r.u64());
  DetectionPolicy& d = policy.detection;
  d.window_capacity = r.u64();
  d.nominal_sample_interval_ticks = r.u64();
  d.max_sample_gap_ticks = r.u64();
  d.min_samples_for_classification = r.u64();
  d.min_completeness_permille = r.u32();
  d.staleness_limit_ticks = r.u64();
  d.reorder_tolerance_ticks = r.u64();
  d.onset_min_depth = r.u64();
  d.onset_min_slope_q16 = r.i64();
  d.onset_sustain_ticks = r.u64();
  d.onset_slope_window_ticks = r.u64();
  d.release_depth = r.u64();
  d.release_slope_q16 = r.i64();
  d.release_sustain_ticks = r.u64();
  if (!decode_ladder(r, d.occupancy_permille_ladder) || !decode_ladder(r, d.depth_ladder) ||
      !decode_ladder(r, d.slope_ladder) || !decode_ladder(r, d.duration_ladder) ||
      !decode_ladder(r, d.drop_ladder)) {
    return Status::failure(StatusCode::kTruncated, "policy ladder payload is truncated");
  }
  d.onset_merge_ticks = r.u64();
  d.cooldown_ticks = r.u64();
  d.max_episode_ticks = r.u64();
  d.evidence_grace_ticks = r.u64();
  d.allow_degraded_classification = r.u8() != 0;
  d.degraded_min_completeness_permille = r.u32();
  d.degraded_severity_cap = static_cast<SeverityClass>(r.u8());
  d.allow_capacity_change_within_window = r.u8() != 0;
  for (std::size_t i = 0; i < kInterventionKindCount; ++i) {
    if (!decode_kind_limit(r, policy.intervention.kinds[i])) {
      return Status::failure(StatusCode::kTruncated, "policy kind limit payload is truncated");
    }
  }
  policy.intervention.max_concurrent_per_stream = r.u32();
  policy.intervention.max_concurrent_total = r.u32();
  policy.intervention.require_authoritative_evidence = r.u8() != 0;
  if (!r.ok()) {
    return Status::failure(StatusCode::kTruncated, "policy payload is truncated");
  }
  return PolicySet::validate(policy);
}

}  // namespace mbg
