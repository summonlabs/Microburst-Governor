// Microburst Governor - persistence, recovery and restart fencing tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mbg/governor.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/persist/snapshot.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

GovernorConfig durable_config() {
  GovernorConfig config;
  config.policy = make_default_policy();
  config.policy.detection.window_capacity = 32;
  config.policy.detection.min_samples_for_classification = 6;
  config.epoch = Epoch::from_raw(1);
  config.incarnation = IncarnationId::from_raw(0xA1B2C3D4ULL);
  config.boot = BootId::from_raw(1);
  return config;
}

/// A trace that ends while the burst is still in flight, so recovery has an unfinished attempt to
/// classify rather than only committed history.
std::vector<Sample> durable_stream(std::uint64_t resource = 1) {
  synthetic::GeneratorConfig config;
  config.shape = synthetic::TraceShape::kMicroburst;
  config.ticks = 72;
  config.burst_start = 60;
  config.burst_ticks = 10;
  config.resource = ResourceId::from_raw(resource);
  config.queue = QueueId::from_raw(1);
  config.path = PathId::from_raw(1);
  return synthetic::generate(config);
}

void append_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);
  std::vector<std::byte> out(static_cast<std::size_t>(size));
  stream.read(reinterpret_cast<char*>(out.data()), size);
  return out;
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

MBG_TEST(persistence, journal_round_trip_and_verification) {
  const test::TempDir directory("journal");
  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());

  std::vector<std::byte> payload;
  ByteWriter writer(payload);
  writer.u64(0x1122334455667788ULL);
  for (std::uint64_t i = 1; i <= 16; ++i) {
    MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(i),
                             Tick::from_raw(i * 10U), payload)
                    .ok());
  }
  MBG_CHECK_EQ(store.records_appended(), 16U);

  persist::RecoveryReport report;
  std::vector<std::byte> snapshot_payload;
  std::uint64_t seen = 0;
  const Status replayed = store.replay(
      [&seen](const persist::JournaledRecord& record) -> Status {
        if (record.type != persist::RecordType::kCheckpoint) {
          return Status::failure(StatusCode::kCorrupt, "unexpected record type");
        }
        seen += 1;
        return Status{};
      },
      report, snapshot_payload);
  MBG_REQUIRE(replayed.ok());
  MBG_CHECK_EQ(seen, 16U);
  MBG_CHECK(report.outcome == persist::RecoveryOutcome::kClean);
  MBG_CHECK_EQ(report.bytes_discarded, 0U);
}

MBG_TEST(persistence, truncated_tail_is_recovered_and_discarded) {
  const test::TempDir directory("torn");
  const std::string journal = directory.path() + "/mbg-state.journal";
  {
    persist::DurableStore store;
    persist::DurableStore::Config config;
    config.directory = directory.path();
    MBG_REQUIRE(store.open(config).ok());
    std::vector<std::byte> payload;
    ByteWriter writer(payload);
    writer.u64(42);
    for (std::uint64_t i = 1; i <= 4; ++i) {
      MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(i),
                               Tick::from_raw(i), payload)
                      .ok());
    }
  }
  // Simulate a crash in the middle of writing a fifth record.
  std::vector<std::byte> image = read_bytes(journal);
  std::vector<std::byte> torn = persist::framing::build_record(
      persist::RecordType::kCheckpoint, RecordSeq::from_raw(5), Tick::from_raw(5),
      std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}});
  torn.resize(torn.size() / 2U);
  image.insert(image.end(), torn.begin(), torn.end());
  write_bytes(journal, image);

  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());
  persist::RecoveryReport report;
  std::vector<std::byte> snapshot_payload;
  std::uint64_t seen = 0;
  MBG_REQUIRE(store
                  .replay(
                      [&seen](const persist::JournaledRecord&) -> Status {
                        seen += 1;
                        return Status{};
                      },
                      report, snapshot_payload)
                  .ok());
  MBG_CHECK_EQ(seen, 4U);
  MBG_CHECK(report.outcome == persist::RecoveryOutcome::kTruncatedTail);
  MBG_CHECK(report.bytes_discarded > 0);
  // The unverifiable tail was retired so the next append starts from a consistent boundary.
  MBG_CHECK(read_bytes(journal).size() < image.size());
  MBG_CHECK(store.needs_compaction() == false);
}

MBG_TEST(persistence, corrupt_middle_stops_replay) {
  const test::TempDir directory("corrupt");
  const std::string journal = directory.path() + "/mbg-state.journal";
  {
    persist::DurableStore store;
    persist::DurableStore::Config config;
    config.directory = directory.path();
    MBG_REQUIRE(store.open(config).ok());
    std::vector<std::byte> payload;
    ByteWriter writer(payload);
    writer.u64(7);
    for (std::uint64_t i = 1; i <= 3; ++i) {
      MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(i), Tick{},
                               payload)
                      .ok());
    }
  }
  std::vector<std::byte> image = read_bytes(journal);
  const std::size_t first = persist::framing::kRecordHeaderBytes + 8U;
  image[first] = static_cast<std::byte>(static_cast<unsigned>(image[first]) ^ 0xFFU);
  write_bytes(journal, image);

  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());
  persist::RecoveryReport report;
  std::vector<std::byte> snapshot_payload;
  std::uint64_t seen = 0;
  MBG_REQUIRE(store
                  .replay(
                      [&seen](const persist::JournaledRecord&) -> Status {
                        seen += 1;
                        return Status{};
                      },
                      report, snapshot_payload)
                  .ok());
  MBG_CHECK_EQ(seen, 1U);
  MBG_CHECK(report.outcome == persist::RecoveryOutcome::kCorruptTail);
}

MBG_TEST(persistence, snapshot_supersedes_the_journal) {
  const test::TempDir directory("snapshot");
  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());
  std::vector<std::byte> early;
  ByteWriter early_writer(early);
  early_writer.u64(1);
  MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(1), Tick{},
                           early)
                  .ok());
  std::vector<std::byte> snapshot;
  ByteWriter snapshot_writer(snapshot);
  snapshot_writer.u64(0xCAFEBABEULL);
  MBG_REQUIRE(store.write_snapshot(snapshot, RecordSeq::from_raw(1), Tick::from_raw(9)).ok());
  MBG_CHECK_EQ(store.journal_bytes(), 0U);

  std::vector<std::byte> after;
  ByteWriter after_writer(after);
  after_writer.u64(2);
  MBG_REQUIRE(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(2),
                           Tick::from_raw(20), after)
                  .ok());

  persist::RecoveryReport report;
  std::vector<std::byte> loaded;
  std::uint64_t seen = 0;
  MBG_REQUIRE(store
                  .replay(
                      [&seen](const persist::JournaledRecord&) -> Status {
                        seen += 1;
                        return Status{};
                      },
                      report, loaded)
                  .ok());
  MBG_CHECK(report.snapshot_present);
  MBG_CHECK_EQ(report.snapshot_covered_seq, 1U);
  MBG_CHECK_EQ(loaded.size(), snapshot.size());
  MBG_CHECK_EQ(seen, 1U);  // the pre snapshot record is superseded
}

MBG_TEST(persistence, governor_restart_fences_authority_and_keeps_history) {
  const test::TempDir directory("restart");
  std::size_t history_size = 0;
  {
    persist::DurableStore store;
    persist::DurableStore::Config config;
    config.directory = directory.path();
    MBG_REQUIRE(store.open(config).ok());

    GovernorConfig governor_config = durable_config();
    Governor governor(governor_config);
    MBG_REQUIRE(governor.bind_store(&store).ok());
    for (const Sample& sample : durable_stream()) {
      static_cast<void>(governor.ingest(sample));
    }
    MBG_REQUIRE(!governor.live_interventions().empty());
    MBG_REQUIRE(governor.open_episodes().size() == 1U);
    MBG_REQUIRE(governor.checkpoint().ok());
    history_size = governor.event_history().size();
  }

  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());

  GovernorConfig governor_config = durable_config();
  governor_config.epoch = Epoch::from_raw(2);
  governor_config.incarnation = IncarnationId::from_raw(0xDEADBEEFULL);
  governor_config.boot = BootId::from_raw(2);
  Governor governor(governor_config);
  MBG_REQUIRE(governor.bind_store(&store).ok());
  persist::RestoreReport report;
  MBG_REQUIRE(governor.restore(report).ok());
  MBG_CHECK(report.snapshot_present);
  MBG_CHECK(report.configuration_items >= 1);
  MBG_CHECK(report.history_items >= history_size);
  MBG_CHECK(report.evidence_requiring_revalidation >= 1);
  MBG_CHECK(governor.live_interventions().empty());
  MBG_CHECK_EQ(governor.stream_count(), 0U);
  MBG_CHECK(!governor.event_history().empty());

  // After recovery the governor accepts fresh evidence and can classify again.
  GovernorConfig second = durable_config();
  second.incarnation = IncarnationId::from_raw(0xDEADBEEFULL);
  second.epoch = Epoch::from_raw(2);
  for (const Sample& sample : durable_stream(2)) {
    static_cast<void>(governor.ingest(sample));
  }
  MBG_CHECK(governor.stats().episodes_opened >= 1);
}

MBG_TEST(persistence, journal_only_recovery_reconstructs_history) {
  const test::TempDir directory("journal-only");
  {
    persist::DurableStore store;
    persist::DurableStore::Config config;
    config.directory = directory.path();
    MBG_REQUIRE(store.open(config).ok());
    GovernorConfig governor_config = durable_config();
    Governor governor(governor_config);
    MBG_REQUIRE(governor.bind_store(&store).ok());
    for (const Sample& sample : durable_stream()) {
      static_cast<void>(governor.ingest(sample));
    }
    // Deliberately no checkpoint: recovery must reconstruct from the journal alone.
    MBG_CHECK(store.records_appended() > 0);
  }
  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  MBG_REQUIRE(store.open(config).ok());
  GovernorConfig governor_config = durable_config();
  governor_config.epoch = Epoch::from_raw(2);
  governor_config.boot = BootId::from_raw(2);
  Governor governor(governor_config);
  MBG_REQUIRE(governor.bind_store(&store).ok());
  persist::RestoreReport report;
  MBG_REQUIRE(governor.restore(report).ok());
  MBG_CHECK(report.records_applied > 0);
  // The episode never reached a terminal state before the stop, so recovery must classify it as an
  // unfinished attempt rather than restoring it as live.
  MBG_CHECK(report.unfinished_attempts >= 1);
  MBG_CHECK(report.history_items >= 1);
  MBG_CHECK(governor.live_interventions().empty());
  MBG_CHECK(!governor.event_history().empty());
}
