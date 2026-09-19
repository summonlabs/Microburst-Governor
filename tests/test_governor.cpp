// Microburst Governor - governor integration tests.
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

GovernorConfig governor_config(std::uint64_t resource = 1) {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.id = make_policy_id("mbg.governor.test.v1");
  config.policy.detection.window_capacity = 32;
  config.policy.detection.min_samples_for_classification = 6;
  config.policy.detection.onset_min_depth = 1024;
  config.policy.detection.onset_min_slope_q16 = rate_from_whole(64);
  config.policy.detection.onset_sustain_ticks = 2;
  config.policy.detection.release_depth = 256;
  config.policy.detection.release_slope_q16 = rate_from_whole(4);
  config.policy.detection.release_sustain_ticks = 2;
  config.policy.detection.cooldown_ticks = 8;
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0xABCDEF01ULL);
  config.boot = BootId::from_raw(1);
  static_cast<void>(resource);
  return config;
}

std::vector<Sample> burst(std::uint64_t resource = 1, std::uint64_t ticks = 200) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = ticks;
  config.burst_start = 60;
  config.burst_ticks = 10;
  config.resource = ResourceId::from_raw(resource);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  return synthetic::generate(config);
}

/// Ingests only the samples up to and including the supplied tick, so an episode is still open when
/// the caller inspects it.
std::vector<Sample> truncated_burst(std::uint64_t resource = 1, std::uint64_t last_tick = 70) {
  std::vector<Sample> samples = burst(resource);
  samples.erase(std::remove_if(samples.begin(), samples.end(),
                               [last_tick](const Sample& sample) {
                                 return sample.tick.value() > last_tick;
                               }),
                samples.end());
  return samples;
}

}  // namespace

MBG_TEST(governor, detects_and_requests_bounded_intent) {
  Governor governor(governor_config());
  for (const Sample& sample : burst()) {
    static_cast<void>(governor.ingest(sample));
  }
  const GovernorStats stats = governor.stats();
  MBG_CHECK_EQ(stats.episodes_opened, 1U);
  MBG_CHECK(stats.interventions_requested >= 1);
  // The queue drains inside the trace, so the single episode has already closed by the end of it.
  MBG_CHECK(governor.open_episodes().empty());
  MBG_REQUIRE(governor.event_history().size() == 1U);
  MBG_CHECK(governor.event_history().front().close_reason == CloseReason::kReleaseConfirmed);
  MBG_CHECK(governor.event_history().front().positive());

  for (const InterventionIntent& intent : governor.live_interventions()) {
    MBG_CHECK(intent.magnitude_permille > 0);
    MBG_CHECK(intent.magnitude_permille <= limits::kMaxMagnitudePermille);
    MBG_CHECK(intent.live());
    MBG_CHECK(intent.authority.epoch == governor.epoch());
    MBG_CHECK(intent.authority.incarnation == governor.incarnation());
    MBG_CHECK(intent.authority.policy_generation == governor.policy_generation());
    MBG_CHECK(intent.justification_severity != SeverityClass::kNone);
  }
}

MBG_TEST(governor, interventions_are_revoked_when_the_episode_ends) {
  Governor governor(governor_config());
  const std::vector<Sample> samples = truncated_burst();
  for (const Sample& sample : samples) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_REQUIRE(!governor.open_episodes().empty());
  MBG_REQUIRE(!governor.live_interventions().empty());
  static_cast<void>(governor.advance(Tick::from_raw(samples.back().tick.value() + 4096U)));
  MBG_CHECK(governor.live_interventions().empty());
  MBG_CHECK(governor.open_episodes().empty());
  bool revoked_seen = false;
  for (const InterventionIntent& intent : governor.intervention_history()) {
    if (intent.state == InterventionState::kRevoked || intent.state == InterventionState::kExpired) {
      revoked_seen = true;
      MBG_CHECK(intent.revocation_verdict != AuthorityVerdict::kValid);
    }
  }
  MBG_CHECK(revoked_seen);
}

MBG_TEST(governor, policy_change_revokes_live_authority) {
  Governor governor(governor_config());
  for (const Sample& sample : truncated_burst()) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_REQUIRE(!governor.live_interventions().empty());
  const PolicyGeneration before = governor.policy_generation();
  Policy changed = make_default_policy();
  changed.detection.onset_min_depth = 2048;
  MBG_REQUIRE(governor.install_policy(changed).ok());
  MBG_CHECK(governor.policy_generation() != before);
  MBG_CHECK(governor.live_interventions().empty());
  for (const InterventionIntent& intent : governor.intervention_history()) {
    if (intent.state == InterventionState::kRevoked) {
      MBG_CHECK(intent.revocation_verdict == AuthorityVerdict::kPolicyChanged);
    }
  }
}

MBG_TEST(governor, authority_rebind_revokes_live_authority) {
  Governor governor(governor_config());
  for (const Sample& sample : truncated_burst()) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_REQUIRE(!governor.live_interventions().empty());
  MBG_REQUIRE(governor
                  .bind_authority(Epoch::from_raw(2), IncarnationId::from_raw(0x11223344ULL),
                                  BootId::from_raw(2))
                  .ok());
  MBG_CHECK(governor.live_interventions().empty());
  MBG_CHECK(governor.epoch() == Epoch::from_raw(2));
}

MBG_TEST(governor, intervention_concurrency_is_bounded) {
  GovernorConfig config = governor_config();
  config.policy.intervention.max_concurrent_per_stream = 1;
  config.policy.intervention.max_concurrent_total = 2;
  Governor governor(config);
  for (std::uint64_t resource = 1; resource <= 4; ++resource) {
    for (const Sample& sample : burst(resource)) {
      static_cast<void>(governor.ingest(sample));
    }
  }
  MBG_CHECK(governor.live_interventions().size() <= 2U);
}

MBG_TEST(governor, emissions_are_bounded_and_drainable) {
  Governor governor(governor_config());
  for (const Sample& sample : burst()) {
    static_cast<void>(governor.ingest(sample));
  }
  const std::vector<Emission> emissions = governor.drain_emissions();
  MBG_CHECK(!emissions.empty());
  MBG_CHECK(governor.drain_emissions().empty());
  bool opened = false;
  for (const Emission& emission : emissions) {
    if (emission.kind == EmissionKind::kEpisodeOpened) {
      opened = true;
    }
  }
  MBG_CHECK(opened);
}

MBG_TEST(governor, one_episode_identity_per_burst_across_streams) {
  Governor governor(governor_config());
  const std::vector<Sample> samples = burst();
  std::vector<EventId> identities;
  for (const Sample& sample : samples) {
    const IngestOutcome outcome = governor.ingest(sample);
    if (outcome.decision.opened) {
      identities.push_back(outcome.decision.event);
    }
  }
  MBG_CHECK_EQ(identities.size(), 1U);
  MBG_CHECK(identities.front().valid());

  // Replaying the same evidence into a fresh governor produces the identical identity.
  Governor replay(governor_config());
  std::vector<EventId> replayed;
  for (const Sample& sample : samples) {
    const IngestOutcome outcome = replay.ingest(sample);
    if (outcome.decision.opened) {
      replayed.push_back(outcome.decision.event);
    }
  }
  MBG_CHECK_EQ(replayed.size(), 1U);
  MBG_CHECK(replayed.front() == identities.front());
}

MBG_TEST(governor, rejected_samples_never_mutate_authoritative_state) {
  Governor governor(governor_config());
  Sample malformed;
  malformed.depth = 5000;
  malformed.occupancy_bytes = 999999;
  malformed.capacity_bytes = 100;
  const IngestOutcome outcome = governor.ingest(malformed);
  MBG_CHECK(!outcome.status.ok());
  MBG_CHECK(!is_positive(outcome.classification));
  MBG_CHECK_EQ(governor.stats().decisions, 0U);
  MBG_CHECK_EQ(governor.stats().samples_rejected, 1U);
}

MBG_TEST(governor, interventions_disabled_means_no_intent) {
  GovernorConfig config = governor_config();
  config.enable_interventions = false;
  Governor governor(config);
  for (const Sample& sample : burst()) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_CHECK(governor.stats().episodes_opened >= 1);
  MBG_CHECK(governor.live_interventions().empty());
  MBG_CHECK_EQ(governor.stats().interventions_requested, 0U);
}

MBG_TEST(governor, resource_generation_change_fences_the_episode) {
  Governor governor(governor_config());
  std::vector<Sample> samples = burst();
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i == 65) {
      // Mid burst: the resource is reconfigured while an episode is open.
      samples[i].resource_generation = ResourceGeneration::from_raw(2);
    }
    static_cast<void>(governor.ingest(samples[i]));
  }
  MBG_CHECK(governor.stats().episodes_fenced >= 1);
  bool fenced_generation = false;
  for (const BurstEvent& event : governor.event_history()) {
    if (event.lifecycle == EventLifecycle::kFenced &&
        event.close_reason == CloseReason::kCapacityChanged) {
      fenced_generation = true;
    }
  }
  MBG_CHECK(fenced_generation);
  MBG_CHECK(governor.open_episodes().empty());
}

MBG_TEST(governor, history_growth_is_bounded) {
  Governor governor(governor_config());
  std::uint64_t tick = 0;
  for (std::uint64_t resource = 0; resource < 64; ++resource) {
    for (const Sample& sample : burst(resource + 1, 128)) {
      Sample shifted = sample;
      shifted.tick = Tick::from_raw(sample.tick.value() + tick);
      static_cast<void>(governor.ingest(shifted));
    }
    tick += 1000;
  }
  MBG_CHECK(governor.event_history().size() <= limits::kMaxEventHistory);
  MBG_CHECK(governor.stream_count() <= limits::kMaxStreams);
}
