// Microburst Governor - concurrency and shutdown tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

GovernorConfig concurrency_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.detection.window_capacity = 32;
  config.policy.detection.min_samples_for_classification = 6;
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0x77665544ULL);
  config.boot = BootId::from_raw(1);
  return config;
}

std::vector<Sample> stream_for(std::uint64_t resource, std::uint64_t ticks) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = ticks;
  config.burst_start = 40;
  config.burst_ticks = 8;
  config.resource = ResourceId::from_raw(resource);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  config.seed = resource;
  return synthetic::generate(config);
}

}  // namespace

MBG_TEST(concurrency, parallel_ingest_from_many_threads) {
  Governor governor(concurrency_config());
  constexpr std::uint64_t kThreads = 8;
  constexpr std::uint64_t kResourcesPerThread = 4;
  constexpr std::uint64_t kTicks = 200;

  std::vector<std::vector<Sample>> streams;
  for (std::uint64_t resource = 1; resource <= kThreads * kResourcesPerThread; ++resource) {
    streams.push_back(stream_for(resource, kTicks));
  }

  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<bool> stop_reader{false};
  std::atomic<std::uint64_t> reader_iterations{0};

  std::vector<std::thread> writers;
  writers.reserve(kThreads);
  for (std::uint64_t thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&, thread]() {
      for (std::uint64_t index = 0; index < kResourcesPerThread; ++index) {
        const std::vector<Sample>& samples =
            streams[static_cast<std::size_t>(thread * kResourcesPerThread + index)];
        for (const Sample& sample : samples) {
          const IngestOutcome outcome = governor.ingest(sample);
          if (outcome.status.ok()) {
            accepted.fetch_add(1);
          } else {
            rejected.fetch_add(1);
          }
        }
        static_cast<void>(governor.advance(samples.back().tick));
      }
    });
  }

  std::thread reader([&]() {
    while (!stop_reader.load()) {
      static_cast<void>(governor.stats());
      static_cast<void>(governor.event_history());
      static_cast<void>(governor.open_episodes());
      static_cast<void>(governor.live_interventions());
      static_cast<void>(governor.drain_emissions());
      reader_iterations.fetch_add(1);
    }
  });

  for (std::thread& writer : writers) {
    writer.join();
  }
  stop_reader.store(true);
  reader.join();

  MBG_CHECK(reader_iterations.load() > 0);
  MBG_CHECK_EQ(rejected.load(), 0U);
  MBG_CHECK_EQ(accepted.load(), kThreads * kResourcesPerThread * (kTicks + 1));
  MBG_CHECK_EQ(governor.stream_count(), kThreads * kResourcesPerThread);
  MBG_CHECK(governor.stats().episodes_opened >= 1);

  // Shutdown path: advancing time after all writers stopped must close every episode cleanly and
  // leave no live authority behind.
  static_cast<void>(governor.advance(Tick::from_raw(1000000)));
  MBG_CHECK(governor.open_episodes().empty());
  MBG_CHECK(governor.live_interventions().empty());
}

MBG_TEST(concurrency, policy_changes_race_with_ingest) {
  Governor governor(concurrency_config());
  const std::vector<Sample> samples = stream_for(1, 400);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> failures{0};

  std::thread writer([&]() {
    for (const Sample& sample : samples) {
      if (!governor.ingest(sample).status.ok()) {
        failures.fetch_add(1);
      }
    }
  });
  std::thread mutator([&]() {
    std::uint64_t round = 0;
    while (!stop.load()) {
      Policy policy = make_default_policy();
      policy.detection.onset_min_depth = 1024 + (round % 8U) * 16U;
      if (!governor.install_policy(policy).ok()) {
        failures.fetch_add(1);
      }
      round += 1;
    }
  });
  writer.join();
  stop.store(true);
  mutator.join();

  MBG_CHECK_EQ(failures.load(), 0U);
  MBG_CHECK(governor.policy_generation().raw() > 0);
}

MBG_TEST(concurrency, cancellation_leaves_no_live_authority) {
  Governor governor(concurrency_config());
  const std::vector<Sample> all = stream_for(1, 200);
  // Stop while the burst is still in flight so there is live authority to cancel.
  for (const Sample& sample : all) {
    if (sample.tick.value() > 48U) {
      break;
    }
    static_cast<void>(governor.ingest(sample));
  }
  MBG_REQUIRE(!governor.live_interventions().empty());
  governor.set_interventions_enabled(false);
  MBG_CHECK(governor.live_interventions().empty());
  // Re-enabling does not resurrect the cancelled intents.
  governor.set_interventions_enabled(true);
  MBG_CHECK(governor.live_interventions().empty());
}
