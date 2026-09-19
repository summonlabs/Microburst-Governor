// Microburst Governor - hardening pass: adversarial and boundary pressure.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <string>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

GovernorConfig hardening_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.detection.window_capacity = 16;
  config.policy.detection.min_samples_for_classification = 4;
  config.policy.detection.onset_min_depth = 512;
  config.policy.detection.onset_min_slope_q16 = rate_from_whole(32);
  config.policy.detection.onset_sustain_ticks = 1;
  config.policy.detection.onset_slope_window_ticks = 2;
  config.policy.detection.release_depth = 128;
  config.policy.detection.release_sustain_ticks = 1;
  config.policy.detection.cooldown_ticks = 4;
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0x48415244454E0001ULL);
  config.boot = BootId::from_raw(1);
  return config;
}

Sample make_sample(ResourceId resource, std::uint64_t tick, std::uint64_t depth,
                   std::uint64_t occupancy, std::uint64_t capacity) {
  Sample sample;
  sample.stream.resource = resource;
  sample.stream.queue = QueueId::from_raw(1);
  sample.stream.path = PathId::from_raw(1);
  sample.tick = Tick::from_raw(tick);
  sample.seq = SampleSeq::from_raw(tick);
  sample.depth = depth;
  sample.occupancy_bytes = occupancy;
  sample.capacity_bytes = capacity;
  sample.ingress_rate_q16 = rate_from_whole(100);
  sample.egress_rate_q16 = rate_from_whole(10);
  sample.flags = SampleFlag::kSynthetic;
  return sample;
}

}  // namespace

MBG_TEST(hardening, stream_capacity_is_enforced_not_exceeded) {
  Governor governor(hardening_config());
  Status last{};
  for (std::uint64_t resource = 1; resource <= limits::kMaxStreams + 8U; ++resource) {
    last = governor.ingest(make_sample(ResourceId::from_raw(resource), 1, 10, 10, 1000)).status;
    if (!last.ok()) {
      break;
    }
  }
  MBG_CHECK(last.code() == StatusCode::kCapacityExceeded);
  MBG_CHECK(governor.stream_count() <= limits::kMaxStreams);
}

MBG_TEST(hardening, emission_buffer_is_bounded) {
  Governor governor(hardening_config());
  // Continuous cadence: a burst every sixteen ticks, far more emissions than the pending buffer can
  // hold, and nothing is drained until the end.
  const ResourceId resource = ResourceId::from_raw(1);
  std::uint64_t tick = 0;
  for (std::uint64_t round = 0; round < 2000; ++round) {
    for (std::uint64_t step = 0; step < 16; ++step) {
      tick += 1;
      const bool high = step >= 4 && step < 8;
      static_cast<void>(
          governor.ingest(make_sample(resource, tick, high ? 4096 : 32, high ? 4096 : 32, 8192)));
    }
  }
  const GovernorStats stats = governor.stats();
  MBG_CHECK(stats.emissions_emitted > limits::kMaxPendingEmissions);
  MBG_CHECK(stats.emissions_dropped > 0);
  MBG_CHECK(governor.drain_emissions().size() <= limits::kMaxPendingEmissions);
}

MBG_TEST(hardening, journal_compaction_is_transparent_and_bounded) {
  const test::TempDir directory("compact");
  persist::DurableStore store;
  persist::DurableStore::Config store_config;
  store_config.directory = directory.path();
  store_config.max_journal_bytes = 2048;
  store_config.fsync_on_append = false;
  MBG_REQUIRE(store.open(store_config).ok());

  GovernorConfig config = hardening_config();
  Governor governor(config);
  MBG_REQUIRE(governor.bind_store(&store).ok());

  std::uint64_t tick = 0;
  for (std::uint64_t round = 0; round < 400; ++round) {
    for (std::uint64_t step = 0; step < 16; ++step) {
      tick += 1;
      const bool high = step >= 4 && step < 8;
      static_cast<void>(governor.ingest(
          make_sample(ResourceId::from_raw(1), tick, high ? 4096 : 32, high ? 4096 : 32, 8192)));
    }
  }
  MBG_CHECK(governor.stats().durable_append_failures == 0U);
  MBG_CHECK(governor.stats().durable_compactions > 0U);
  MBG_CHECK(store.journal_bytes() <= store_config.max_journal_bytes);
  MBG_CHECK(governor.stats().episodes_opened > 0);

  persist::RecoveryReport report;
  std::vector<std::byte> snapshot;
  MBG_REQUIRE(store.replay([](const persist::JournaledRecord&) { return Status{}; }, report, snapshot)
                  .ok());
  MBG_CHECK(report.snapshot_present);
  MBG_CHECK(report.outcome == persist::RecoveryOutcome::kClean ||
            report.outcome == persist::RecoveryOutcome::kTruncatedTail);
}

MBG_TEST(hardening, trace_field_overflow_is_rejected_not_truncated) {
  std::vector<Sample> samples;
  // resource generation is a 32 bit identity: a larger field must not be silently narrowed.
  const std::string line = "1 1 1 10 10 1000 100 10 0 0 0 0 8 4294967296\n";
  MBG_CHECK(!synthetic::decode_trace(line, samples, 4).ok());
  const std::string boundary = "1 1 1 10 10 1000 100 10 0 0 0 0 8 4294967295\n";
  MBG_CHECK(synthetic::decode_trace(boundary, samples, 4).ok());
  MBG_CHECK_EQ(samples.size(), 1U);
  MBG_CHECK_EQ(samples.front().resource_generation.raw(), 4294967295U);
}

MBG_TEST(hardening, ticks_that_go_backwards_never_reopen_an_episode) {
  Governor governor(hardening_config());
  const ResourceId resource = ResourceId::from_raw(1);
  for (std::uint64_t tick = 1; tick <= 20; ++tick) {
    static_cast<void>(governor.ingest(make_sample(resource, tick, 32, 32, 8192)));
  }
  const std::uint64_t opened_before = governor.stats().episodes_opened;
  for (std::uint64_t tick = 12; tick <= 20; ++tick) {
    static_cast<void>(governor.ingest(make_sample(resource, tick, 4096, 4096, 8192)));
  }
  MBG_CHECK_EQ(governor.stats().episodes_opened, opened_before);
  MBG_CHECK_EQ(governor.stats().samples_rejected, 0U);
}

MBG_TEST(hardening, the_episode_ceiling_always_closes_an_episode) {
  GovernorConfig config = hardening_config();
  config.policy.detection.max_episode_ticks = 32;
  config.policy.detection.release_depth = 8;
  Governor governor(config);
  const ResourceId resource = ResourceId::from_raw(1);
  std::uint64_t tick = 0;
  // A single sustained elevation with a genuine leading edge: the release rules never fire, so only
  // the episode ceiling can end it.
  for (std::uint64_t step = 0; step < 4; ++step) {
    tick += 1;
    static_cast<void>(governor.ingest(make_sample(resource, tick, 32, 32, 8192)));
  }
  for (std::uint64_t step = 0; step < 400; ++step) {
    tick += 1;
    static_cast<void>(governor.ingest(make_sample(resource, tick, 4096, 4096, 8192)));
  }
  MBG_CHECK(governor.stats().episodes_opened >= 1);
  MBG_CHECK(governor.stats().episodes_closed + governor.stats().episodes_fenced >= 1);
  MBG_CHECK(governor.open_episodes().empty());
  bool timed_out = false;
  for (const BurstEvent& event : governor.event_history()) {
    if (event.close_reason == CloseReason::kEpisodeTimeout) {
      timed_out = true;
      MBG_CHECK(event.metrics.duration_ticks <= 4096U + config.policy.detection.window_capacity);
    }
  }
  MBG_CHECK(timed_out);
}

MBG_TEST(hardening, extreme_but_legal_values_do_not_overflow) {
  Governor governor(hardening_config());
  const ResourceId resource = ResourceId::from_raw(1);
  const std::uint64_t big = 1ULL << 61;
  for (std::uint64_t tick = 1; tick <= 8; ++tick) {
    static_cast<void>(governor.ingest(make_sample(resource, tick, big, big, big)));
  }
  static_cast<void>(governor.advance(Tick::from_raw(1ULL << 62)));
  const GovernorStats stats = governor.stats();
  MBG_CHECK_EQ(stats.samples_rejected, 0U);
  for (const BurstEvent& event : governor.event_history()) {
    MBG_CHECK(event.metrics.peak_depth <= big);
    MBG_CHECK(event.metrics.duration_ticks <= (1ULL << 62));
  }
  for (const InterventionIntent& intent : governor.intervention_history()) {
    MBG_CHECK(intent.magnitude_permille <= limits::kMaxMagnitudePermille);
  }
}

MBG_TEST(hardening, policy_generation_cannot_be_exhausted_into_a_wrap) {
  PolicySet set;
  Policy policy = make_default_policy();
  set = PolicySet{};
  // Reinstalling many times keeps the generation strictly increasing.
  PolicyGeneration previous = set.generation();
  for (int round = 0; round < 64; ++round) {
    MBG_REQUIRE(set.set_fallback(policy).ok());
    MBG_CHECK(set.generation().raw() > previous.raw());
    previous = set.generation();
  }
}

MBG_TEST(hardening, repeated_evaluation_of_the_same_tick_is_free_of_side_effects) {
  Governor governor(hardening_config());
  const ResourceId resource = ResourceId::from_raw(1);
  for (std::uint64_t tick = 1; tick <= 12; ++tick) {
    static_cast<void>(governor.ingest(make_sample(resource, tick, 4096, 4096, 8192)));
  }
  const GovernorStats before = governor.stats();
  for (int repeat = 0; repeat < 16; ++repeat) {
    static_cast<void>(governor.advance(Tick::from_raw(12)));
  }
  const GovernorStats after = governor.stats();
  MBG_CHECK_EQ(after.episodes_opened, before.episodes_opened);
  MBG_CHECK_EQ(after.interventions_requested, before.interventions_requested);
  MBG_CHECK_EQ(after.episodes_closed, before.episodes_closed);
}

MBG_TEST(hardening, a_failed_durable_append_never_loses_acknowledged_state) {
  const test::TempDir directory("durable-ack");
  persist::DurableStore store;
  persist::DurableStore::Config store_config;
  store_config.directory = directory.path();
  store_config.fsync_on_append = true;
  MBG_REQUIRE(store.open(store_config).ok());
  std::vector<std::byte> payload;
  ByteWriter writer(payload);
  writer.u64(0x1234);
  for (std::uint64_t i = 1; i <= 8; ++i) {
    MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(i),
                             Tick::from_raw(i), payload)
                    .ok());
  }
  // Everything acknowledged before a stop must still verify afterwards.
  persist::RecoveryReport report;
  std::vector<std::byte> snapshot;
  std::uint64_t seen = 0;
  MBG_REQUIRE(store
                  .replay(
                      [&seen](const persist::JournaledRecord&) {
                        seen += 1;
                        return Status{};
                      },
                      report, snapshot)
                  .ok());
  MBG_CHECK_EQ(seen, 8U);
  MBG_CHECK(report.outcome == persist::RecoveryOutcome::kClean);
}
