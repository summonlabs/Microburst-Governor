// Microburst Governor - mbgctl verify.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include "cli_common.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/persist/snapshot.hpp"

namespace mbg::cli {

int run_verify(const Arguments& arguments) {
  const std::optional<std::string> directory = arguments.value("--store");
  if (!directory.has_value()) {
    std::fputs("mbgctl verify: --store DIR is required\n", stderr);
    return 2;
  }
  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = *directory;
  config.fsync_on_append = false;
  const Status opened = store.open(config);
  if (!opened.ok()) {
    std::fprintf(stderr, "mbgctl verify: store could not be opened: %s\n", opened.detail());
    return 2;
  }

  persist::RecoveryReport report;
  const Status verified = store.verify(report);
  if (!verified.ok()) {
    std::fprintf(stderr, "mbgctl verify: integrity check failed: %s (%s)\n", verified.detail(),
                 to_string(verified.code()));
    return 1;
  }
  std::printf("verify: outcome=%s snapshot=%s records=%llu bytes_valid=%llu bytes_discarded=%llu\n",
              persist::to_string(report.outcome), report.snapshot_present ? "present" : "absent",
              static_cast<unsigned long long>(report.records_replayed),
              static_cast<unsigned long long>(report.bytes_valid),
              static_cast<unsigned long long>(report.bytes_discarded));

  std::vector<std::byte> snapshot_payload;
  persist::RecoveryReport replay_report;
  std::uint64_t restorable_events = 0;
  std::uint64_t stale_interventions = 0;
  const Status replayed = store.replay(
      [&stale_interventions](const persist::JournaledRecord& record) -> Status {
        if (record.type == persist::RecordType::kInterventionRequested ||
            record.type == persist::RecordType::kInterventionStateChanged) {
          stale_interventions += 1;
        }
        return Status{};
      },
      replay_report, snapshot_payload);
  if (!replayed.ok()) {
    std::fprintf(stderr, "mbgctl verify: replay failed: %s\n", replayed.detail());
    return 1;
  }
  if (report.snapshot_present && !snapshot_payload.empty()) {
    persist::PersistedState state;
    const Status decoded = persist::decode(snapshot_payload, state);
    if (!decoded.ok()) {
      std::fprintf(stderr, "mbgctl verify: snapshot rejected: %s\n", decoded.detail());
      return 1;
    }
    restorable_events = state.events.size();
    stale_interventions += state.interventions.size();
  }
  std::printf(
      "verify: restorable_events=%llu stale_live_interventions=%llu recovery_outcome=%s\n",
      static_cast<unsigned long long>(restorable_events),
      static_cast<unsigned long long>(stale_interventions),
      persist::to_string(replay_report.outcome));
  if (replay_report.outcome == persist::RecoveryOutcome::kCorruptTail ||
      replay_report.outcome == persist::RecoveryOutcome::kVersionMismatch ||
      replay_report.outcome == persist::RecoveryOutcome::kOversized) {
    std::puts("verify: durable state carries damage that is not a plain truncated tail");
    return 1;
  }
  return 0;
}

}  // namespace mbg::cli
