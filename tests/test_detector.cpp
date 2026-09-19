// Microburst Governor - detector rule tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <vector>

#include "mbg/detect/detector.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

const StreamKey kStream{ResourceId::from_raw(1), QueueId::from_raw(1), PathId::from_raw(1)};

Policy test_policy() {
  Policy policy = make_default_policy();
  policy.id = make_policy_id("mbg.test.v1");
  policy.detection.window_capacity = 32;
  policy.detection.min_samples_for_classification = 6;
  policy.detection.min_completeness_permille = 800;
  policy.detection.onset_min_depth = 1024;
  policy.detection.onset_min_slope_q16 = rate_from_whole(64);
  policy.detection.onset_sustain_ticks = 2;
  policy.detection.release_depth = 256;
  policy.detection.release_slope_q16 = rate_from_whole(4);
  policy.detection.release_sustain_ticks = 2;
  policy.detection.staleness_limit_ticks = 8;
  policy.detection.max_sample_gap_ticks = 4;
  policy.detection.cooldown_ticks = 16;
  policy.detection.max_episode_ticks = 4096;
  policy.detection.evidence_grace_ticks = 4;
  return policy;
}

synthetic::GeneratorConfig burst_config(synthetic::TraceShape shape) {
  synthetic::GeneratorConfig config;
  config.shape = shape;
  config.ticks = 200;
  config.burst_start = 60;
  config.burst_ticks = 10;
  config.resource = kStream.resource;
  config.queue = kStream.queue;
  config.path = kStream.path;
  return config;
}

struct RunResult {
  std::vector<DetectionDecision> decisions;
  std::uint64_t positives{0};
  std::uint64_t opened{0};
  std::uint64_t closed{0};
  std::uint64_t fenced{0};
  Classification last{Classification::kUnknown};
  SeverityClass severity{SeverityClass::kNone};
  std::uint64_t max_duration{0};
  std::uint64_t peak_depth{0};
  std::vector<EventId> ids;
};

RunResult run(Detector& detector, const std::vector<Sample>& samples) {
  RunResult result;
  for (const Sample& sample : samples) {
    DetectionDecision decision;
    const Status status = detector.ingest(sample, decision);
    if (!status.ok()) {
      continue;
    }
    result.decisions.push_back(decision);
    result.last = decision.classification;
    if (is_positive(decision.classification)) {
      result.positives += 1;
      result.severity = decision.severity;
    }
    if (decision.opened) {
      result.opened += 1;
      result.ids.push_back(decision.event);
    }
    if (decision.closed) {
      result.closed += 1;
      result.fenced = decision.fenced ? result.fenced + 1 : result.fenced;
    }
    if (decision.metrics.duration_ticks > result.max_duration) {
      result.max_duration = decision.metrics.duration_ticks;
    }
    if (decision.metrics.peak_depth > result.peak_depth) {
      result.peak_depth = decision.metrics.peak_depth;
    }
  }
  return result;
}

}  // namespace

MBG_TEST(detector, detects_a_synthetic_microburst) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  const RunResult result = run(detector, synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst)));
  MBG_CHECK(result.opened >= 1);
  MBG_CHECK(result.positives >= 1);
  MBG_CHECK(severity_at_least(result.severity, SeverityClass::kMinor));
  MBG_CHECK(severity_at_least(result.severity, SeverityClass::kModerate));
  MBG_CHECK_EQ(result.peak_depth, 9216U);
}

MBG_TEST(detector, steady_and_noisy_traffic_is_not_a_microburst) {
  for (const synthetic::TraceShape shape :
       {synthetic::TraceShape::kSteady, synthetic::TraceShape::kNoisySteady,
        synthetic::TraceShape::kShallowNoise}) {
    Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
    const RunResult result = run(detector, synthetic::generate(burst_config(shape)));
    MBG_CHECK_EQ(result.opened, 0U);
    MBG_CHECK(!is_positive(result.last));
  }
}

MBG_TEST(detector, one_episode_per_burst_and_a_cooldown_after_it) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  const RunResult result =
      run(detector, synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst)));
  MBG_CHECK_EQ(result.opened, 1U);
  MBG_CHECK(result.ids.size() == 1U);
}

MBG_TEST(detector, an_oscillating_burst_stays_bounded) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  synthetic::GeneratorConfig config = burst_config(synthetic::TraceShape::kOscillating);
  config.burst_ticks = 8;
  config.ticks = 400;
  const RunResult result = run(detector, synthetic::generate(config));
  // The cooldown bounds how many episodes a pathological oscillation can open.
  const std::uint64_t upper_bound = config.ticks / test_policy().detection.cooldown_ticks + 2U;
  MBG_CHECK(result.opened <= upper_bound);
  MBG_CHECK(result.opened >= 1U);
}

MBG_TEST(detector, stale_evidence_is_never_positive) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  const auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
  DetectionDecision decision;
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, decision));
  }
  const Tick far_future = Tick::from_raw(samples.back().tick.value() + 10000U);
  static_cast<void>(detector.advance(far_future, decision));
  MBG_CHECK(!is_positive(decision.classification));
  MBG_CHECK(decision.classification == Classification::kFenced ||
            decision.classification == Classification::kUnknown);
  MBG_CHECK(decision.explanation.contains(ReasonCode::kEvidenceStale) ||
            decision.explanation.contains(ReasonCode::kEpisodeFenced));
}

MBG_TEST(detector, incomplete_evidence_is_unknown) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  DetectionDecision decision;
  synthetic::GeneratorConfig config = burst_config(synthetic::TraceShape::kMicroburst);
  config.sample_interval = 16;  // far coarser than the policy nominal cadence
  const auto samples = synthetic::generate(config);
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, decision));
  }
  MBG_CHECK(!is_positive(decision.classification));
  MBG_CHECK(decision.classification == Classification::kUnknown ||
            decision.classification == Classification::kQuiescent ||
            decision.classification == Classification::kCandidate);
  MBG_CHECK(decision.authority == EvidenceAuthority::kUnknown ||
            decision.authority == EvidenceAuthority::kAuthoritative);
}

MBG_TEST(detector, degraded_policy_allows_incomplete_but_never_stale) {
  Policy policy = test_policy();
  policy.detection.allow_degraded_classification = true;
  policy.detection.degraded_min_completeness_permille = 100;
  policy.detection.degraded_severity_cap = SeverityClass::kMinor;
  policy.detection.min_samples_for_classification = 8;
  Detector detector(kStream, policy, PolicyGeneration::from_raw(1));

  synthetic::GeneratorConfig config = burst_config(synthetic::TraceShape::kMicroburst);
  config.sample_interval = 4;
  const auto samples = synthetic::generate(config);
  DetectionDecision decision;
  bool degraded_seen = false;
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, decision));
    if (decision.classification == Classification::kDegradedMicroburst) {
      degraded_seen = true;
      MBG_CHECK(decision.authority == EvidenceAuthority::kDegraded);
      MBG_CHECK(!severity_at_least(decision.severity, SeverityClass::kSevere));
      MBG_CHECK(severity_at_least(policy.detection.degraded_severity_cap, decision.severity));
    }
  }
  MBG_CHECK(degraded_seen);

  // Staleness is never relaxed, not even by a degraded policy.
  static_cast<void>(detector.advance(Tick::from_raw(samples.back().tick.value() + 100000U), decision));
  MBG_CHECK(!is_positive(decision.classification));
}

MBG_TEST(detector, release_rules_close_the_episode) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  const auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
  DetectionDecision decision;
  std::uint64_t closes = 0;
  std::uint64_t recoveries = 0;
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, decision));
    if (decision.classification == Classification::kRecovering) {
      recoveries += 1;
    }
    if (decision.closed) {
      closes += 1;
      MBG_CHECK(decision.close_reason == CloseReason::kReleaseConfirmed);
      MBG_CHECK(decision.metrics.duration_ticks > 0);
      // A closed episode immediately enters its cooldown, which is what bounds oscillation.
      MBG_CHECK(detector.in_cooldown(decision.tick));
    }
  }
  // The queue drains inside the trace, so the episode recovers and then closes exactly once.
  MBG_CHECK(recoveries > 0);
  MBG_CHECK_EQ(closes, 1U);
  MBG_CHECK(!detector.has_open_episode());
  // Well past the cooldown, a fresh burst can be admitted again.
  MBG_CHECK(!detector.in_cooldown(Tick::from_raw(samples.back().tick.value())));
}

MBG_TEST(detector, policy_rearm_fences_and_clears_evidence) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
  // Keep only the leading edge of the burst, while the episode is still open.
  samples.erase(std::remove_if(samples.begin(), samples.end(),
                               [](const Sample& sample) { return sample.tick.value() > 71U; }),
                samples.end());
  DetectionDecision decision;
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, decision));
  }
  MBG_REQUIRE(detector.has_open_episode());
  Policy changed = test_policy();
  changed.detection.onset_min_depth = 2048;
  const auto fenced = detector.rearm(changed, PolicyGeneration::from_raw(2), samples.back().tick);
  MBG_REQUIRE(fenced.has_value());
  MBG_CHECK(fenced->fenced);
  MBG_CHECK(fenced->close_reason == CloseReason::kPolicyChanged);
  MBG_CHECK_EQ(detector.window().size(), 0U);
  MBG_CHECK(!detector.has_open_episode());
}

MBG_TEST(detector, evaluation_is_idempotent_at_a_repeated_tick) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  const auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
  DetectionDecision first;
  for (const Sample& sample : samples) {
    static_cast<void>(detector.ingest(sample, first));
  }
  DetectionDecision second;
  static_cast<void>(detector.advance(samples.back().tick, second));
  MBG_CHECK(first.explanation.digest() == second.explanation.digest());
  MBG_CHECK_EQ(first.event.raw(), second.event.raw());
  MBG_CHECK_EQ(static_cast<int>(first.classification), static_cast<int>(second.classification));
}

MBG_TEST(detector, classification_is_deterministic_across_runs) {
  std::uint64_t first_digest = 0;
  std::uint64_t second_digest = 0;
  for (int pass = 0; pass < 2; ++pass) {
    Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
    const auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
    DetectionDecision decision;
    std::uint64_t digest = 0;
    for (const Sample& sample : samples) {
      static_cast<void>(detector.ingest(sample, decision));
      digest = hash_combine(digest, decision.explanation.digest());
      digest = hash_combine(digest, static_cast<std::uint64_t>(decision.classification));
      digest = hash_combine(digest, decision.event.raw());
    }
    if (pass == 0) {
      first_digest = digest;
    } else {
      second_digest = digest;
    }
  }
  MBG_CHECK_EQ(first_digest, second_digest);
}

MBG_TEST(detector, capacity_change_fences_the_episode) {
  Detector detector(kStream, test_policy(), PolicyGeneration::from_raw(1));
  auto samples = synthetic::generate(burst_config(synthetic::TraceShape::kMicroburst));
  DetectionDecision decision;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i == 100 && detector.has_open_episode()) {
      samples[i].resource_generation = ResourceGeneration::from_raw(2);
    }
    static_cast<void>(detector.ingest(samples[i], decision));
  }
  const bool fenced = detector.state() == DetectorState::kIdle &&
                      decision.close_reason == CloseReason::kCapacityChanged;
  MBG_CHECK(fenced || !detector.has_open_episode());
}
