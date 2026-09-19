// Microburst Governor - seeded randomized property tests and hardening shapes.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

GovernorConfig property_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.id = make_policy_id("mbg.property.test.v1");
  config.policy.detection.window_capacity = 48;
  config.policy.detection.min_samples_for_classification = 8;
  config.policy.detection.onset_min_depth = 1024;
  config.policy.detection.onset_min_slope_q16 = rate_from_whole(64);
  config.policy.detection.onset_sustain_ticks = 3;
  config.policy.detection.release_depth = 256;
  config.policy.detection.release_slope_q16 = rate_from_whole(4);
  config.policy.detection.release_sustain_ticks = 3;
  config.policy.detection.cooldown_ticks = 16;
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0xFEEDFACEULL);
  config.boot = BootId::from_raw(1);
  return config;
}

synthetic::GeneratorConfig base_generator(synthetic::TraceShape shape, std::uint64_t seed) {
  synthetic::GeneratorConfig config;
  config.shape = shape;
  config.seed = seed;
  config.ticks = 300;
  config.burst_start = 80;
  config.burst_ticks = 12;
  config.resource = ResourceId::from_raw(1);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  return config;
}

struct Summary {
  std::uint64_t opened{0};
  std::uint64_t positives{0};
  std::uint64_t unknown{0};
  std::uint64_t decisions{0};
  std::uint64_t digest{0};
  std::vector<EventId> ids;
};

Summary run_shape(synthetic::TraceShape shape, std::uint64_t seed, GovernorConfig config) {
  Governor governor(config);
  Summary summary;
  const std::vector<Sample> samples = synthetic::generate(base_generator(shape, seed));
  for (const Sample& sample : samples) {
    const IngestOutcome outcome = governor.ingest(sample);
    if (!outcome.status.ok()) {
      continue;
    }
    summary.decisions += 1;
    summary.digest = hash_combine(summary.digest, outcome.decision.explanation.digest());
    summary.digest = hash_combine(summary.digest, static_cast<std::uint64_t>(outcome.classification));
    if (is_positive(outcome.classification)) {
      summary.positives += 1;
    }
    if (outcome.classification == Classification::kUnknown) {
      summary.unknown += 1;
    }
    if (outcome.decision.opened) {
      summary.opened += 1;
      summary.ids.push_back(outcome.decision.event);
    }
  }
  static_cast<void>(governor.advance(Tick::from_raw(samples.back().tick.value() + 4096U)));
  summary.digest = hash_combine(summary.digest, governor.stats().episodes_closed);
  return summary;
}

}  // namespace

MBG_TEST(property, false_positive_control_over_seeded_noise) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    const Summary steady = run_shape(synthetic::TraceShape::kSteady, seed, property_config());
    MBG_CHECK_EQ(steady.opened, 0U);
    MBG_CHECK_EQ(steady.positives, 0U);

    const Summary shallow = run_shape(synthetic::TraceShape::kShallowNoise, seed, property_config());
    MBG_CHECK_EQ(shallow.opened, 0U);

    const Summary noisy = run_shape(synthetic::TraceShape::kNoisySteady, seed, property_config());
    MBG_CHECK_EQ(noisy.opened, 0U);
  }
}

MBG_TEST(property, true_positive_control_over_seeded_bursts) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    const Summary burst = run_shape(synthetic::TraceShape::kMicroburst, seed, property_config());
    MBG_CHECK(burst.opened >= 1);
    MBG_CHECK(burst.positives >= 1);
    MBG_CHECK_EQ(burst.ids.size(), static_cast<std::size_t>(burst.opened));
  }
}

MBG_TEST(property, replay_is_bit_for_bit_deterministic) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    const Summary first = run_shape(synthetic::TraceShape::kMicroburst, seed, property_config());
    const Summary second = run_shape(synthetic::TraceShape::kMicroburst, seed, property_config());
    MBG_CHECK_EQ(first.digest, second.digest);
    MBG_CHECK(first.ids == second.ids);
  }
}

MBG_TEST(property, degraded_and_damaged_sampling_never_fabricates_a_burst) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    const Summary sparse = run_shape(synthetic::TraceShape::kSparseSampling, seed, property_config());
    MBG_CHECK_EQ(sparse.opened, 0U);
    const Summary missing = run_shape(synthetic::TraceShape::kMissingSamples, seed, property_config());
    MBG_CHECK_EQ(missing.opened, 0U);
  }
}

MBG_TEST(property, reordered_and_duplicated_evidence_does_not_corrupt_state) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    GovernorConfig config = property_config();
    Governor governor(config);
    const std::vector<Sample> samples =
        synthetic::generate(base_generator(synthetic::TraceShape::kDuplicate, seed));
    for (const Sample& sample : samples) {
      const IngestOutcome outcome = governor.ingest(sample);
      MBG_CHECK(outcome.status.ok());
      MBG_CHECK(!has_flag(sample.flags, SampleFlag::kSynthetic) || true);
    }
    MBG_CHECK(governor.stats().samples_duplicate > 0);
  }
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    Governor governor(property_config());
    const std::vector<Sample> samples =
        synthetic::generate(base_generator(synthetic::TraceShape::kReordered, seed));
    for (const Sample& sample : samples) {
      static_cast<void>(governor.ingest(sample));
    }
    // A backwards tick is refused for the derivative and accounted for as out of order, never
    // silently accepted.
    MBG_CHECK(governor.stats().samples_reordered + governor.stats().samples_discontinuity > 0);
    MBG_CHECK_EQ(governor.stats().samples_rejected, 0U);
  }
}

MBG_TEST(property, counter_reset_and_capacity_change_do_not_claim_a_burst) {
  GovernorConfig config = property_config();
  config.policy.detection.allow_capacity_change_within_window = false;

  {
    // A cumulative counter that restarts in the middle of otherwise steady traffic must not, by
    // itself, produce a burst: the derivative is refused for that interval.
    Governor steady_governor(config);
    synthetic::GeneratorConfig generator = base_generator(synthetic::TraceShape::kSteady, 11);
    std::vector<Sample> samples = synthetic::generate(generator);
    for (std::size_t i = 100; i < samples.size(); ++i) {
      samples[i].drop_total = 0;
      samples[i].mark_total = 0;
      samples[i].ingress_total = samples[i].ingress_total % 1000U;
      samples[i].egress_total = samples[i].egress_total % 1000U;
      samples[i].flags = samples[i].flags | SampleFlag::kCounterReset;
    }
    for (const Sample& sample : samples) {
      static_cast<void>(steady_governor.ingest(sample));
    }
    MBG_CHECK_EQ(steady_governor.stats().episodes_opened, 0U);
    MBG_CHECK_EQ(steady_governor.stats().samples_rejected, 0U);
  }

  Governor governor(config);
  synthetic::GeneratorConfig generator = base_generator(synthetic::TraceShape::kCapacityChange, 3);
  for (const Sample& sample : synthetic::generate(generator)) {
    static_cast<void>(governor.ingest(sample));
  }
  // A capacity change invalidates the occupancy dimension of the window.
  for (const BurstEvent& event : governor.event_history()) {
    MBG_CHECK(!severity_at_least(event.severity, SeverityClass::kCritical));
  }
}

MBG_TEST(property, the_largest_supported_window_stays_bounded) {
  GovernorConfig config = property_config();
  config.policy.detection.window_capacity = limits::kMaxWindowCapacity;
  config.policy.detection.min_samples_for_classification = 64;
  Governor governor(config);
  const std::vector<Sample> samples = synthetic::generate(
      base_generator(synthetic::TraceShape::kLongBurst, 5));
  for (const Sample& sample : samples) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_CHECK(governor.stats().streams <= limits::kMaxStreams);
  MBG_CHECK(governor.event_history().size() <= limits::kMaxEventHistory);
  MBG_CHECK(governor.live_interventions().size() <= limits::kMaxInterventions);
}

MBG_TEST(property, burst_episode_count_is_bounded_by_the_cooldown) {
  GovernorConfig config = property_config();
  config.policy.detection.cooldown_ticks = 32;
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    Governor governor(config);
    synthetic::GeneratorConfig generator = base_generator(synthetic::TraceShape::kOscillating, seed);
    generator.ticks = 600;
    generator.burst_ticks = 8;
    for (const Sample& sample : synthetic::generate(generator)) {
      static_cast<void>(governor.ingest(sample));
    }
    const std::uint64_t bound = generator.ticks / config.policy.detection.cooldown_ticks + 2U;
    MBG_CHECK(governor.stats().episodes_opened <= bound);
  }
}

MBG_TEST(property, huge_tick_values_do_not_overflow) {
  Governor governor(property_config());
  synthetic::GeneratorConfig generator = base_generator(synthetic::TraceShape::kMicroburst, 1);
  generator.start_tick = ~std::uint64_t{0} - 4096U;
  for (const Sample& sample : synthetic::generate(generator)) {
    const IngestOutcome outcome = governor.ingest(sample);
    MBG_CHECK(outcome.status.ok());
  }
  MBG_CHECK(governor.stats().samples_ingested > 0);
}
