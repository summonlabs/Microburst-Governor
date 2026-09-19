// Microburst Governor - policy and evidence window tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "mbg/detect/quantities.hpp"
#include "mbg/detect/window.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

Sample make_sample(std::uint64_t tick, std::uint64_t depth, std::uint64_t occupancy,
                   std::uint64_t capacity, ResourceId resource = ResourceId::from_raw(1)) {
  Sample sample;
  sample.stream.resource = resource;
  sample.stream.queue = QueueId::from_raw(1);
  sample.tick = Tick::from_raw(tick);
  sample.seq = SampleSeq::from_raw(tick);
  sample.depth = depth;
  sample.occupancy_bytes = occupancy;
  sample.capacity_bytes = capacity;
  sample.ingress_rate_q16 = rate_from_whole(100);
  sample.egress_rate_q16 = rate_from_whole(50);
  return sample;
}

}  // namespace

MBG_TEST(policy, defaults_are_valid) {
  const Policy policy = make_default_policy();
  MBG_CHECK(PolicySet::validate(policy).ok());
  MBG_CHECK(policy.digest() != 0U);
  // The default policy is conservative: it does not request headroom changes.
  MBG_CHECK(!policy.intervention
                 .kinds[static_cast<std::size_t>(InterventionKind::kTemporaryHeadroomIncrease)]
                 .enabled);
}

MBG_TEST(policy, validation_rejects_unbounded_and_incoherent_settings) {
  Policy policy = make_default_policy();
  policy.detection.window_capacity = limits::kMaxWindowCapacity + 1U;
  MBG_CHECK(PolicySet::validate(policy).code() == StatusCode::kOutOfRange);

  policy = make_default_policy();
  policy.detection.release_depth = policy.detection.onset_min_depth;
  MBG_CHECK(PolicySet::validate(policy).code() == StatusCode::kInvalidArgument);

  policy = make_default_policy();
  policy.detection.min_completeness_permille = 1001;
  MBG_CHECK(!PolicySet::validate(policy).ok());

  policy = make_default_policy();
  policy.detection.occupancy_permille_ladder.thresholds[0] = 2000;
  MBG_CHECK(!PolicySet::validate(policy).ok());

  policy = make_default_policy();
  policy.detection.window_capacity = 16;
  policy.detection.min_samples_for_classification = 32;
  MBG_CHECK(!PolicySet::validate(policy).ok());

  policy = make_default_policy();
  policy.detection.allow_degraded_classification = true;
  policy.detection.degraded_severity_cap = SeverityClass::kCritical;
  MBG_CHECK(!PolicySet::validate(policy).ok());

  policy = make_default_policy();
  policy.intervention.kinds[0].magnitude_permille = limits::kMaxMagnitudePermille + 1U;
  MBG_CHECK(!PolicySet::validate(policy).ok());

  policy = make_default_policy();
  policy.intervention.max_concurrent_per_stream = 10;
  policy.intervention.max_concurrent_total = 2;
  MBG_CHECK(!PolicySet::validate(policy).ok());
}

MBG_TEST(policy, generation_and_digest_track_mutation) {
  PolicySet set;
  const PolicyGeneration first = set.generation();
  const std::uint64_t first_digest = set.digest();
  MBG_CHECK(first.raw() != 0);

  Policy override_policy = make_default_policy();
  override_policy.detection.onset_min_depth = 4096;
  MBG_CHECK(set.set_override(ResourceId::from_raw(9), override_policy).ok());
  MBG_CHECK(set.generation() != first);
  MBG_CHECK(set.digest() != first_digest);
  MBG_CHECK_EQ(set.override_count(), 1U);
  MBG_CHECK(set.resolve(ResourceId::from_raw(9)).detection.onset_min_depth == 4096U);
  MBG_CHECK(set.resolve(ResourceId::from_raw(10)).detection.onset_min_depth ==
            make_default_policy().detection.onset_min_depth);

  // Replacing the override with an identical policy still advances the generation: an installed
  // policy document is a new authority even when its content matches.
  const PolicyGeneration before_replace = set.generation();
  MBG_CHECK(set.set_override(ResourceId::from_raw(9), override_policy).ok());
  MBG_CHECK(set.generation() != before_replace);
  MBG_CHECK_EQ(set.override_count(), 1U);

  MBG_CHECK(set.remove_override(ResourceId::from_raw(9)).ok());
  MBG_CHECK_EQ(set.override_count(), 0U);
  MBG_CHECK(set.remove_override(ResourceId::from_raw(9)).code() == StatusCode::kNotFound);
}

MBG_TEST(policy, restore_rejects_corrupt_override_sets) {
  PolicySet set;
  Policy fallback = make_default_policy();
  std::vector<PolicyOverride> overrides;
  overrides.push_back(PolicyOverride{ResourceId::from_raw(1), fallback});
  overrides.push_back(PolicyOverride{ResourceId::from_raw(1), fallback});
  MBG_CHECK(PolicySet::restore(fallback, overrides, PolicyGeneration::from_raw(3), set).code() ==
            StatusCode::kCorrupt);
  MBG_CHECK(PolicySet::restore(fallback, {}, PolicyGeneration::from_raw(0), set).code() ==
            StatusCode::kInvalidArgument);
}

MBG_TEST(policy, ladder_grading_is_ordered) {
  GradeLadder ladder;
  ladder.thresholds = {10, 20, 30, 40};
  MBG_CHECK(ladder.monotonic());
  MBG_CHECK(ladder.grade(0) == SeverityClass::kNone);
  MBG_CHECK(ladder.grade(10) == SeverityClass::kMinor);
  MBG_CHECK(ladder.grade(25) == SeverityClass::kModerate);
  MBG_CHECK(ladder.grade(1000) == SeverityClass::kCritical);
  MBG_CHECK(ladder.grade_rate(-5) == SeverityClass::kNone);
  GradeLadder unordered;
  unordered.thresholds = {40, 10, 20, 30};
  MBG_CHECK(!unordered.monotonic());
}

MBG_TEST(window, admits_only_monotonic_ticks) {
  EvidenceWindow window(8);
  DetectionPolicy policy;
  MBG_CHECK_EQ(static_cast<int>(window.offer(make_sample(5, 10, 10, 100), 0)),
               static_cast<int>(WindowAccept::kAccepted));
  MBG_CHECK_EQ(static_cast<int>(window.offer(make_sample(5, 10, 10, 100), 0)),
               static_cast<int>(WindowAccept::kDuplicate));
  MBG_CHECK_EQ(static_cast<int>(window.offer(make_sample(6, 20, 20, 100), 0)),
               static_cast<int>(WindowAccept::kAccepted));
  MBG_CHECK_EQ(static_cast<int>(window.offer(make_sample(4, 10, 10, 100), 0)),
               static_cast<int>(WindowAccept::kDiscontinuity));
  MBG_CHECK_EQ(window.size(), 2U);
  MBG_CHECK_EQ(window.duplicates(), 1U);
  MBG_CHECK_EQ(window.discontinuities(), 1U);

  const EvidenceWindow::Metrics metrics = window.metrics(policy);
  MBG_CHECK_EQ(metrics.samples, 2U);
  MBG_CHECK_EQ(metrics.latest_depth, 20U);
  MBG_CHECK_EQ(metrics.completeness_permille, 1000U);
  MBG_CHECK(metrics.recent_slope_q16 == rate_from_whole(10));
}

MBG_TEST(window, completeness_reflects_sampling_gaps) {
  EvidenceWindow window(32);
  DetectionPolicy policy;
  policy.nominal_sample_interval_ticks = 1;
  // Two samples ten ticks apart in a window that expects eleven.
  static_cast<void>(window.offer(make_sample(0, 10, 10, 100), 0));
  static_cast<void>(window.offer(make_sample(10, 10, 10, 100), 0));
  const EvidenceWindow::Metrics metrics = window.metrics(policy);
  MBG_CHECK_EQ(metrics.expected_samples, 11U);
  MBG_CHECK_EQ(metrics.completeness_permille, 181U);
  MBG_CHECK_EQ(metrics.max_gap_ticks, 10U);
}

MBG_TEST(window, capacity_change_is_flagged) {
  EvidenceWindow window(8);
  DetectionPolicy policy;
  static_cast<void>(window.offer(make_sample(1, 10, 10, 100), 0));
  const WindowAccept accept = window.offer(make_sample(2, 60, 60, 200), 0);
  MBG_CHECK_EQ(static_cast<int>(accept), static_cast<int>(WindowAccept::kCapacityChanged));
  const EvidenceWindow::Metrics metrics = window.metrics(policy);
  MBG_CHECK(metrics.capacity_changed);
  MBG_CHECK_EQ(metrics.capacity_bytes, 200U);
  MBG_CHECK_EQ(metrics.peak_depth, 60U);
}

MBG_TEST(window, bounded_capacity_never_grows) {
  EvidenceWindow window(4);
  for (std::uint64_t i = 0; i < 100; ++i) {
    static_cast<void>(window.offer(make_sample(i, i, i, 1000), 0));
  }
  MBG_CHECK_EQ(window.size(), 4U);
  MBG_CHECK_EQ(window.oldest().depth, 96U);
  MBG_CHECK_EQ(window.newest().depth, 99U);
  MBG_CHECK_EQ(window.accepted_total(), 100U);
}

MBG_TEST(window, counter_rollover_is_carried_through_the_window) {
  EvidenceWindow window(8);
  DetectionPolicy policy;
  Sample first = make_sample(1, 10, 10, 100);
  first.drop_total = ~std::uint64_t{0} - 1U;
  first.mark_total = 0;
  Sample second = make_sample(2, 20, 20, 100);
  second.drop_total = 4;
  second.mark_total = 11;
  static_cast<void>(window.offer(first, 0));
  static_cast<void>(window.offer(second, 0));
  const EvidenceWindow::Metrics metrics = window.metrics(policy);
  // (2^64 - 2) -> 2^64 - 1 -> 0 -> 4 is six forward steps.
  MBG_CHECK_EQ(metrics.drop_delta, 6U);
  MBG_CHECK_EQ(metrics.mark_delta, 11U);
}

MBG_TEST(window, synthetic_traces_are_labelled) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = 64;
  const std::vector<Sample> samples = synthetic::generate(config);
  MBG_CHECK(!samples.empty());
  for (const Sample& sample : samples) {
    MBG_CHECK(has_flag(sample.flags, SampleFlag::kSynthetic));
  }
  EvidenceWindow window(64);
  DetectionPolicy policy;
  for (const Sample& sample : samples) {
    static_cast<void>(window.offer(sample, 0));
  }
  MBG_CHECK(window.metrics(policy).synthetic);
}

MBG_TEST(window, trace_lines_round_trip_and_reject_damage) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kSteady;
  config.ticks = 32;
  const std::vector<Sample> samples = synthetic::generate(config);
  const std::string text = synthetic::encode_trace(samples);
  std::vector<Sample> parsed;
  MBG_REQUIRE(synthetic::decode_trace(text, parsed, 1024).ok());
  MBG_CHECK_EQ(parsed.size(), samples.size());
  MBG_CHECK(parsed[3].depth == samples[3].depth);
  MBG_CHECK(parsed[3].tick == samples[3].tick);

  std::vector<Sample> rejected;
  MBG_CHECK(!synthetic::decode_trace("1 2 3\n", rejected, 16).ok());
  MBG_CHECK(!synthetic::decode_trace("1 2 3 4 5 6 7 8 9 10 11 12 13 99 15\n", rejected, 16).ok());
  MBG_CHECK(!synthetic::decode_trace("1 2 3 4 5 6 seven 8 9 10 11 12 13 1 15\n", rejected, 16).ok());
  MBG_CHECK(synthetic::decode_trace(text, rejected, 2).code() == StatusCode::kOversized);
}
