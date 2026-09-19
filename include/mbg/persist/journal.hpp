// Microburst Governor - crash safe durable store.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/tick.hpp"

namespace mbg::persist {

/// Record kinds written to the journal. Values are persisted, so they are never renumbered.
enum class RecordType : std::uint16_t {
  kUnknown = 0,
  kAuthorityBound = 1,   ///< the authority this incarnation started with
  kPolicyInstalled = 2,  ///< a policy document became authoritative
  kEpisodeOpened = 3,
  kEpisodeUpdated = 4,
  kEpisodeClosed = 5,
  kEpisodeFenced = 6,
  kInterventionRequested = 7,
  kInterventionStateChanged = 8,
  kCheckpoint = 9,
};

[[nodiscard]] const char* to_string(RecordType type) noexcept;
[[nodiscard]] bool is_valid_record_type(std::uint16_t raw) noexcept;

/// How recovery classified the journal tail.
enum class RecoveryOutcome : std::uint8_t {
  kClean = 0,          ///< every record verified
  kTruncatedTail = 1,  ///< the last record was partially written (the expected crash artefact)
  kCorruptTail = 2,    ///< the last record failed a framing or checksum check
  kMissing = 3,        ///< no durable state exists yet
  kVersionMismatch = 4,
  kOversized = 5,
};

[[nodiscard]] const char* to_string(RecoveryOutcome outcome) noexcept;

struct RecoveryReport {
  RecoveryOutcome outcome{RecoveryOutcome::kMissing};
  std::uint16_t format_version{0};
  std::uint64_t records_replayed{0};
  std::uint64_t bytes_valid{0};
  std::uint64_t bytes_discarded{0};
  std::uint64_t snapshot_covered_seq{0};
  bool snapshot_present{false};
  std::string detail{};
};

/// A verified journal record.
struct JournaledRecord {
  RecordType type{RecordType::kUnknown};
  RecordSeq seq{};
  Tick tick{};
  std::vector<std::byte> payload{};
};

/// Append-only, integrity checked, crash safe store.
///
/// Mutation protocol: validate -> bind authority -> plan -> reserve -> journal -> verify -> commit ->
/// retire. A record is only acknowledged after it is durable, and recovery truncates a partially
/// written tail instead of guessing at it.
///
/// Locking: the store owns one mutex and never calls out to another component while holding it.
/// Replay reads the journal into memory first and invokes the visitor afterwards, so a visitor that
/// touches other components cannot deadlock against this store.
class DurableStore {
 public:
  struct Config {
    std::filesystem::path directory{};
    std::uint64_t max_journal_bytes{limits::kMaxJournalBytes};
    bool fsync_on_append{true};
    std::string journal_name{"mbg-state.journal"};
    std::string snapshot_name{"mbg-state.snapshot"};
  };

  DurableStore() = default;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  ~DurableStore();

  /// Creates the directory if needed and opens (or creates) the journal for appending.
  [[nodiscard]] Status open(Config config);

  [[nodiscard]] const Config& config() const noexcept { return config_; }
  [[nodiscard]] bool is_open() const noexcept { return journal_handle_ != nullptr; }
  [[nodiscard]] std::uint64_t journal_bytes() const;

  /// True when the journal can no longer accept a record: either the byte budget is exhausted or an
  /// append was refused for capacity. The caller must checkpoint and compact before appending again.
  [[nodiscard]] bool needs_compaction() const;
  [[nodiscard]] std::uint64_t records_appended() const;
  [[nodiscard]] RecordSeq last_sequence() const;
  [[nodiscard]] std::uint64_t syncs() const;

  /// Appends a framed record and makes it durable before returning.
  [[nodiscard]] Status append(RecordType type, RecordSeq seq, Tick tick,
                              std::span<const std::byte> payload);

  /// Flushes user-space buffers to the operating system and, when configured, to stable storage.
  [[nodiscard]] Status flush();

  /// Writes the snapshot atomically (temporary file, flush, rename) and truncates the journal.
  [[nodiscard]] Status write_snapshot(std::span<const std::byte> payload, RecordSeq covered_seq,
                                      Tick tick);

  /// Replays the snapshot and every verified journal record, in append order.
  [[nodiscard]] Status replay(const std::function<Status(const JournaledRecord&)>& visitor,
                              RecoveryReport& report, std::vector<std::byte>& snapshot_payload);

  /// Verifies integrity without applying anything.
  [[nodiscard]] Status verify(RecoveryReport& report) const;

  /// Truncates the journal to zero length after a snapshot has been committed.
  [[nodiscard]] Status reset_journal();

 private:
  [[nodiscard]] Status scan_journal(std::vector<std::byte>& bytes, std::uint64_t& valid_end,
                                    RecoveryOutcome& outcome, std::string& detail) const;
  [[nodiscard]] Status load_snapshot(RecoveryReport& report,
                                     std::vector<std::byte>& payload) const;
  [[nodiscard]] Status truncate_journal(std::uint64_t length);
  [[nodiscard]] Status reset_journal_locked();
  [[nodiscard]] std::filesystem::path journal_path() const;
  [[nodiscard]] std::filesystem::path snapshot_path() const;

  Config config_{};
  void* journal_handle_{nullptr};
  std::uint64_t journal_bytes_{0};
  std::uint64_t records_appended_{0};
  std::uint64_t syncs_{0};
  bool compaction_required_{false};
  RecordSeq last_sequence_{};
  mutable std::mutex mutex_{};
};

/// Framing constants, exposed so that tests can construct hostile frames deliberately.
namespace framing {

inline constexpr std::size_t kRecordHeaderBytes = 44;
inline constexpr std::size_t kSnapshotHeaderBytes = 40;
inline constexpr std::size_t kSnapshotTrailerBytes = 4;

/// Byte ranges covered by each header checksum. The checksum covers every header field that
/// precedes it, so the coverage is the header size minus the header checksum word and any checksum
/// word that follows it.
inline constexpr std::size_t kRecordHeaderCrcCoverage = 36;
inline constexpr std::size_t kSnapshotHeaderCrcCoverage = 32;

[[nodiscard]] std::span<const std::byte> record_magic() noexcept;
[[nodiscard]] std::span<const std::byte> snapshot_magic() noexcept;

/// Builds a complete journal record image (header + payload).
[[nodiscard]] std::vector<std::byte> build_record(RecordType type, RecordSeq seq, Tick tick,
                                                  std::span<const std::byte> payload);

/// Builds a complete snapshot image (header + payload + trailer).
[[nodiscard]] std::vector<std::byte> build_snapshot(std::span<const std::byte> payload,
                                                    RecordSeq covered_seq, Tick tick);

}  // namespace framing

}  // namespace mbg::persist
