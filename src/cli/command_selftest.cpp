// Microburst Governor - mbgctl selftest.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "cli_common.hpp"
#include "mbg/governor.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/synthetic/generator.hpp"

namespace mbg::cli {
namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "selftest: FAIL %s\n", what);
    failures += 1;
  }
}

/// One end to end pass over synthetic evidence: detection, classification, bounded intent and
/// hysteresis. Every step is deterministic, so a failure here is reproducible.
void run_evidence_pass() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.incarnation = IncarnationId::from_raw(0x5E1F7E57ULL);
  config.boot = BootId::from_raw(1);
  Governor governor(config);
  check(governor.interventions_enabled(), "interventions start enabled");

  synthetic::GeneratorConfig generator;
  generator.shape = synthetic::TraceShape::kMicroburst;
  generator.ticks = 400;
  generator.burst_start = 100;
  generator.burst_ticks = 12;
  const std::vector<Sample> samples = synthetic::generate(generator);
  check(!samples.empty(), "synthetic trace is not empty");

  bool detected = false;
  for (const Sample& sample : samples) {
    const IngestOutcome outcome = governor.ingest(sample);
    if (is_positive(outcome.classification)) {
      detected = true;
    }
  }
  check(detected, "a synthetic microburst is detected");

  const GovernorStats stats = governor.stats();
  check(stats.episodes_opened >= 1, "an episode was opened");
  check(stats.interventions_requested >= 1, "a bounded intervention was requested");

  for (const InterventionIntent& intent : governor.live_interventions()) {
    check(intent.magnitude_permille <= limits::kMaxMagnitudePermille,
          "intervention magnitude stays bounded");
  }

  static_cast<void>(governor.advance(Tick::from_raw(samples.back().tick.value() + 8192U)));
  const GovernorStats after = governor.stats();
  check(after.episodes_closed >= 1, "the episode closes after the release window");
  check(governor.live_interventions().empty(),
        "no intervention survives the episode that justified it");
}

/// Evidence that stops arriving must never become a positive classification.
void run_stale_pass() {
  GovernorConfig config;
  config.policy = make_default_policy();
  Governor governor(config);
  // The trace stops while the burst is still in flight, so the episode is open when evidence stops.
  synthetic::GeneratorConfig generator;
  generator.shape = synthetic::TraceShape::kMicroburst;
  generator.ticks = 112;
  generator.burst_start = 100;
  generator.burst_ticks = 20;
  const std::vector<Sample> samples = synthetic::generate(generator);
  for (const Sample& sample : samples) {
    static_cast<void>(governor.ingest(sample));
  }
  check(!governor.open_episodes().empty(), "the truncated trace leaves an episode open");
  const Tick last = samples.back().tick;
  const AdvanceResult advanced = governor.advance(Tick::from_raw(last.value() + 100000U));
  check(advanced.streams_evaluated >= 1, "the idle stream is still evaluated");
  check(governor.open_episodes().empty(), "a stale stream must not keep an episode open");
  check(governor.stats().episodes_fenced >= 1, "the stale episode is fenced");
}

/// Deterministic identity: the same evidence replayed twice yields the same episode identities.
void run_determinism_pass() {
  synthetic::GeneratorConfig generator;
  generator.shape = synthetic::TraceShape::kMicroburst;
  generator.ticks = 400;
  generator.burst_start = 100;
  generator.burst_ticks = 12;
  const std::vector<Sample> samples = synthetic::generate(generator);

  std::vector<EventId> first;
  std::vector<EventId> second;
  for (int pass = 0; pass < 2; ++pass) {
    GovernorConfig config;
    config.policy = make_default_policy();
    Governor governor(config);
    for (const Sample& sample : samples) {
      static_cast<void>(governor.ingest(sample));
    }
    static_cast<void>(governor.advance(Tick::from_raw(samples.back().tick.value() + 8192U)));
    std::vector<EventId>& target = pass == 0 ? first : second;
    for (const BurstEvent& event : governor.event_history()) {
      target.push_back(event.id);
    }
  }
  check(!first.empty(), "the deterministic pass produced an episode");
  check(first == second, "replay produces identical episode identities");
}

/// Durable state survives a hard stop, and live authority does not survive with it.
void run_restart_pass() {
  // The self test never writes into the caller working directory: temporary state belongs in the
  // system temporary directory and is removed on every exit path below.
  std::error_code ec;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(ec) / "mbgctl-selftest-store";
  if (ec) {
    check(false, "a temporary directory is available");
    return;
  }
  std::filesystem::remove_all(directory, ec);

  std::size_t events_before = 0;
  Epoch epoch_before{};
  {
    persist::DurableStore store;
    persist::DurableStore::Config config;
    config.directory = directory.string();
    config.fsync_on_append = false;
    const Status opened = store.open(config);
    check(opened.ok(), "durable store opens");
    if (!opened.ok()) {
      std::filesystem::remove_all(directory, ec);
      return;
    }
    GovernorConfig governor_config;
    governor_config.policy = make_default_policy();
    governor_config.epoch = Epoch::from_raw(1);
    governor_config.incarnation = IncarnationId::from_raw(0x1234ULL);
    governor_config.boot = BootId::from_raw(1);
    Governor governor(governor_config);
    static_cast<void>(governor.bind_store(&store));
    check(governor
              .bind_authority(Epoch::from_raw(1), IncarnationId::from_raw(0x1234ULL),
                              BootId::from_raw(1))
              .ok(),
          "authority is bound");

    synthetic::GeneratorConfig generator;
    generator.shape = synthetic::TraceShape::kMicroburst;
    generator.ticks = 400;
    generator.burst_start = 100;
    generator.burst_ticks = 12;
    for (const Sample& sample : synthetic::generate(generator)) {
      static_cast<void>(governor.ingest(sample));
    }
    check(governor.checkpoint().ok(), "checkpoint succeeds");
    events_before = governor.event_history().size();
    epoch_before = governor.epoch();
    check(governor.stats().interventions_requested >= 1, "an intervention existed before the restart");
  }

  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.string();
  config.fsync_on_append = false;
  check(store.open(config).ok(), "durable store reopens");

  GovernorConfig governor_config;
  governor_config.policy = make_default_policy();
  governor_config.epoch = Epoch::from_raw(epoch_before.raw() + 1U);
  governor_config.incarnation = IncarnationId::from_raw(0x5678ULL);
  governor_config.boot = BootId::from_raw(2);
  Governor governor(governor_config);
  static_cast<void>(governor.bind_store(&store));
  persist::RestoreReport report;
  const Status restored = governor.restore(report);
  check(restored.ok(), "restore succeeds");
  check(report.configuration_items >= 1, "durable configuration is restored");
  check(report.history_items >= events_before, "committed history is restored");
  check(report.evidence_requiring_revalidation >= 1, "evidence requires revalidation after restart");
  check(governor.live_interventions().empty(), "no intervention is restored as live");
  check(governor.stream_count() == 0, "no stream liveness is restored");
  std::filesystem::remove_all(directory, ec);
}

}  // namespace

int run_selftest(const Arguments& arguments) {
  static_cast<void>(arguments);
  run_evidence_pass();
  run_stale_pass();
  run_determinism_pass();
  run_restart_pass();
  if (failures != 0) {
    std::fprintf(stderr, "selftest: %d check(s) failed\n", failures);
    return 1;
  }
  std::puts("selftest: ok");
  return 0;
}

}  // namespace mbg::cli
