// Microburst Governor - crash safe durable store.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/persist/journal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>
#include <utility>

#include "mbg/core/bytes.hpp"
#include "mbg/core/checked.hpp"
#include "mbg/core/crc32c.hpp"
#include "mbg/core/hash.hpp"
#include "mbg/version.hpp"

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mbg::persist {
namespace {

constexpr std::array<std::byte, 8> kRecordMagicBytes = {
    std::byte{'M'}, std::byte{'B'}, std::byte{'G'}, std::byte{'J'},
    std::byte{'R'}, std::byte{'N'}, std::byte{'L'}, std::byte{'1'}};
constexpr std::array<std::byte, 8> kSnapshotMagicBytes = {
    std::byte{'M'}, std::byte{'B'}, std::byte{'G'}, std::byte{'S'},
    std::byte{'N'}, std::byte{'A'}, std::byte{'P'}, std::byte{'1'}};

[[nodiscard]] std::span<const std::byte> as_bytes(const void* data, std::size_t length) noexcept {
  return std::span<const std::byte>(static_cast<const std::byte*>(data), length);
}

[[nodiscard]] Status flush_handle(std::FILE* handle, bool sync) {
  if (handle == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store handle is not open");
  }
  if (std::fflush(handle) != 0) {
    return Status::failure(StatusCode::kIo, "failed to flush durable store");
  }
  if (!sync) {
    return Status{};
  }
#if defined(_WIN32)
  if (_commit(_fileno(handle)) != 0) {
    return Status::failure(StatusCode::kIo, "failed to commit durable store to disk");
  }
#else
  if (::fsync(::fileno(handle)) != 0) {
    return Status::failure(StatusCode::kIo, "failed to fsync durable store to disk");
  }
#endif
  return Status{};
}

[[nodiscard]] Status read_file(const std::filesystem::path& path, std::uint64_t maximum,
                               std::vector<std::byte>& out, bool& present) {
  present = false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return Status{};
  }
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status::failure(StatusCode::kIo, "durable store file size is unreadable");
  }
  if (size > maximum) {
    return Status::failure(StatusCode::kOversized, "durable store file exceeds the size bound");
  }
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store file cannot be opened");
  }
  out.assign(static_cast<std::size_t>(size), std::byte{0});
  const std::size_t read = size == 0 ? 0 : std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  if (read != out.size()) {
    return Status::failure(StatusCode::kTruncated, "durable store file shrank while reading");
  }
  present = true;
  return Status{};
}

[[nodiscard]] Status replace_file(const std::filesystem::path& from,
                                  const std::filesystem::path& to) {
  std::error_code ec;
#if defined(_WIN32)
  std::filesystem::rename(from, to, ec);  // overwrites atomically on Windows
#else
  std::filesystem::rename(from, to, ec);
#endif
  if (ec) {
    return Status::failure(StatusCode::kIo, "atomic replace of durable state failed");
  }
  return Status{};
}

}  // namespace

const char* to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::kUnknown: return "unknown";
    case RecordType::kAuthorityBound: return "authority-bound";
    case RecordType::kPolicyInstalled: return "policy-installed";
    case RecordType::kEpisodeOpened: return "episode-opened";
    case RecordType::kEpisodeUpdated: return "episode-updated";
    case RecordType::kEpisodeClosed: return "episode-closed";
    case RecordType::kEpisodeFenced: return "episode-fenced";
    case RecordType::kInterventionRequested: return "intervention-requested";
    case RecordType::kInterventionStateChanged: return "intervention-state-changed";
    case RecordType::kCheckpoint: return "checkpoint";
  }
  return "unknown";
}

bool is_valid_record_type(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(RecordType::kAuthorityBound) &&
         raw <= static_cast<std::uint16_t>(RecordType::kCheckpoint);
}

const char* to_string(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::kClean: return "clean";
    case RecoveryOutcome::kTruncatedTail: return "truncated-tail";
    case RecoveryOutcome::kCorruptTail: return "corrupt-tail";
    case RecoveryOutcome::kMissing: return "missing";
    case RecoveryOutcome::kVersionMismatch: return "version-mismatch";
    case RecoveryOutcome::kOversized: return "oversized";
  }
  return "unknown";
}

namespace framing {

std::span<const std::byte> record_magic() noexcept {
  return std::span<const std::byte>(kRecordMagicBytes.data(), kRecordMagicBytes.size());
}

std::span<const std::byte> snapshot_magic() noexcept {
  return std::span<const std::byte>(kSnapshotMagicBytes.data(), kSnapshotMagicBytes.size());
}

std::vector<std::byte> build_record(RecordType type, RecordSeq seq, Tick tick,
                                    std::span<const std::byte> payload) {
  std::vector<std::byte> out;
  out.reserve(kRecordHeaderBytes + payload.size());
  ByteWriter writer(out);
  writer.raw(kRecordMagicBytes.data(), kRecordMagicBytes.size());
  writer.u16(kStateFormatVersion);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u32(0);  // flags, reserved
  writer.u64(seq.raw());
  writer.u64(tick.value());
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t header_crc = Crc32c::compute(std::span<const std::byte>(out.data(), out.size()));
  writer.u32(header_crc);
  writer.u32(Crc32c::compute(payload));
  writer.raw(payload.data(), payload.size());
  return out;
}

std::vector<std::byte> build_snapshot(std::span<const std::byte> payload, RecordSeq covered_seq,
                                      Tick tick) {
  std::vector<std::byte> out;
  out.reserve(kSnapshotHeaderBytes + payload.size() + kSnapshotTrailerBytes);
  ByteWriter writer(out);
  writer.raw(kSnapshotMagicBytes.data(), kSnapshotMagicBytes.size());
  writer.u16(kStateFormatVersion);
  writer.u16(0);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u64(covered_seq.raw());
  writer.u64(tick.value());
  const std::uint32_t header_crc = Crc32c::compute(std::span<const std::byte>(out.data(), out.size()));
  writer.u32(header_crc);
  writer.u32(Crc32c::compute(payload));
  writer.raw(payload.data(), payload.size());
  writer.u32(Crc32c::compute(std::span<const std::byte>(out.data(), out.size())));
  return out;
}

}  // namespace framing

DurableStore::~DurableStore() {
  if (journal_handle_ != nullptr) {
    std::fflush(static_cast<std::FILE*>(journal_handle_));
    std::fclose(static_cast<std::FILE*>(journal_handle_));
    journal_handle_ = nullptr;
  }
}

std::filesystem::path DurableStore::journal_path() const {
  return config_.directory / config_.journal_name;
}

std::filesystem::path DurableStore::snapshot_path() const {
  return config_.directory / config_.snapshot_name;
}

Status DurableStore::open(Config config) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (config.directory.empty()) {
    return Status::failure(StatusCode::kInvalidArgument, "durable store directory is empty");
  }
  if (config.max_journal_bytes == 0 || config.max_journal_bytes > limits::kMaxJournalBytes) {
    return Status::failure(StatusCode::kOutOfRange, "journal size bound outside supported range");
  }
  if (journal_handle_ != nullptr) {
    return Status::failure(StatusCode::kConflict, "durable store is already open");
  }
  std::error_code ec;
  std::filesystem::create_directories(config.directory, ec);
  if (ec) {
    return Status::failure(StatusCode::kIo, "durable store directory cannot be created");
  }
  config_ = std::move(config);

  const std::string path = journal_path().string();
  std::FILE* handle = std::fopen(path.c_str(), "r+b");
  if (handle == nullptr) {
    handle = std::fopen(path.c_str(), "w+b");
  }
  if (handle == nullptr) {
    return Status::failure(StatusCode::kIo, "journal cannot be opened for append");
  }
  if (std::fseek(handle, 0, SEEK_END) != 0) {
    std::fclose(handle);
    return Status::failure(StatusCode::kIo, "journal cannot be positioned");
  }
  const long position = std::ftell(handle);
  if (position < 0) {
    std::fclose(handle);
    return Status::failure(StatusCode::kIo, "journal length cannot be read");
  }
  journal_handle_ = handle;
  journal_bytes_ = static_cast<std::uint64_t>(position);
  return Status{};
}

std::uint64_t DurableStore::journal_bytes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return journal_bytes_;
}

bool DurableStore::needs_compaction() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return compaction_required_ || journal_bytes_ >= config_.max_journal_bytes;
}

std::uint64_t DurableStore::records_appended() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return records_appended_;
}

RecordSeq DurableStore::last_sequence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_sequence_;
}

std::uint64_t DurableStore::syncs() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return syncs_;
}

Status DurableStore::append(RecordType type, RecordSeq seq, Tick tick,
                            std::span<const std::byte> payload) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_handle_ == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store is not open");
  }
  if (!is_valid_record_type(static_cast<std::uint16_t>(type))) {
    return Status::failure(StatusCode::kInvalidArgument, "journal record type is not writable");
  }
  if (payload.size() > limits::kMaxJournalRecordBytes) {
    return Status::failure(StatusCode::kOversized, "journal record payload exceeds the bound");
  }
  const std::uint64_t record_bytes =
      static_cast<std::uint64_t>(framing::kRecordHeaderBytes) + payload.size();
  const auto projected = add_checked<std::uint64_t>(journal_bytes_, record_bytes);
  if (!projected.has_value() || *projected > config_.max_journal_bytes) {
    compaction_required_ = true;
    return Status::failure(StatusCode::kCapacityExceeded,
                           "journal size bound reached, compaction required");
  }
  const std::vector<std::byte> image = framing::build_record(type, seq, tick, payload);
  std::FILE* handle = static_cast<std::FILE*>(journal_handle_);
  if (std::fwrite(image.data(), 1, image.size(), handle) != image.size()) {
    return Status::failure(StatusCode::kIo, "journal record write failed");
  }
  // The record is only acknowledged once it is durable.
  const Status flushed = flush_handle(handle, config_.fsync_on_append);
  if (!flushed.ok()) {
    return flushed;
  }
  journal_bytes_ = *projected;
  records_appended_ += 1;
  last_sequence_ = seq;
  syncs_ += 1;
  return Status{};
}

Status DurableStore::flush() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_handle_ == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store is not open");
  }
  const Status status = flush_handle(static_cast<std::FILE*>(journal_handle_), config_.fsync_on_append);
  if (status.ok()) {
    syncs_ += 1;
  }
  return status;
}

Status DurableStore::write_snapshot(std::span<const std::byte> payload, RecordSeq covered_seq,
                                    Tick tick) {
  if (payload.size() > limits::kMaxSnapshotBytes) {
    return Status::failure(StatusCode::kOversized, "snapshot payload exceeds the bound");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::byte> image = framing::build_snapshot(payload, covered_seq, tick);
  std::filesystem::path temporary = snapshot_path();
  temporary += ".tmp";
  std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
  if (file == nullptr) {
    return Status::failure(StatusCode::kIo, "snapshot temporary file cannot be created");
  }
  if (std::fwrite(image.data(), 1, image.size(), file) != image.size()) {
    std::fclose(file);
    std::error_code ignore;
    std::filesystem::remove(temporary, ignore);
    return Status::failure(StatusCode::kIo, "snapshot write failed");
  }
  const Status flushed = flush_handle(file, true);
  std::fclose(file);
  if (!flushed.ok()) {
    std::error_code ignore;
    std::filesystem::remove(temporary, ignore);
    return flushed;
  }
  const Status replaced = replace_file(temporary, snapshot_path());
  if (!replaced.ok()) {
    std::error_code ignore;
    std::filesystem::remove(temporary, ignore);
    return replaced;
  }
  syncs_ += 1;
  return reset_journal_locked();
}

Status DurableStore::reset_journal() {
  std::lock_guard<std::mutex> guard(mutex_);
  return reset_journal_locked();
}

Status DurableStore::reset_journal_locked() {
  if (journal_handle_ == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store is not open");
  }
  std::FILE* handle = static_cast<std::FILE*>(journal_handle_);
  if (std::fflush(handle) != 0) {
    return Status::failure(StatusCode::kIo, "journal flush before truncation failed");
  }
  const std::string path = journal_path().string();
  std::fclose(handle);
  journal_handle_ = nullptr;
  std::FILE* truncated = std::fopen(path.c_str(), "wb");
  if (truncated == nullptr) {
    return Status::failure(StatusCode::kIo, "journal cannot be truncated");
  }
  const Status flushed = flush_handle(truncated, true);
  std::fclose(truncated);
  if (!flushed.ok()) {
    return flushed;
  }
  std::FILE* reopened = std::fopen(path.c_str(), "r+b");
  if (reopened == nullptr) {
    return Status::failure(StatusCode::kIo, "journal cannot be reopened after truncation");
  }
  if (std::fseek(reopened, 0, SEEK_END) != 0) {
    std::fclose(reopened);
    return Status::failure(StatusCode::kIo, "journal cannot be repositioned after truncation");
  }
  journal_handle_ = reopened;
  journal_bytes_ = 0;
  compaction_required_ = false;
  return Status{};
}

Status DurableStore::truncate_journal(std::uint64_t length) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (journal_handle_ == nullptr) {
    return Status::failure(StatusCode::kIo, "durable store is not open");
  }
  std::FILE* handle = static_cast<std::FILE*>(journal_handle_);
  if (std::fflush(handle) != 0) {
    return Status::failure(StatusCode::kIo, "journal flush before truncation failed");
  }
#if defined(_WIN32)
  if (_chsize_s(_fileno(handle), static_cast<long long>(length)) != 0) {
    return Status::failure(StatusCode::kIo, "journal truncation failed");
  }
#else
  if (::ftruncate(::fileno(handle), static_cast<off_t>(length)) != 0) {
    return Status::failure(StatusCode::kIo, "journal truncation failed");
  }
#endif
  if (std::fseek(handle, static_cast<long>(length), SEEK_SET) != 0) {
    return Status::failure(StatusCode::kIo, "journal reposition after truncation failed");
  }
  journal_bytes_ = length;
  return flush_handle(handle, config_.fsync_on_append);
}

Status DurableStore::scan_journal(std::vector<std::byte>& bytes, std::uint64_t& valid_end,
                                  RecoveryOutcome& outcome, std::string& detail) const {
  valid_end = 0;
  outcome = RecoveryOutcome::kClean;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < framing::kRecordHeaderBytes) {
      outcome = RecoveryOutcome::kTruncatedTail;
      detail = "journal ends inside a record header";
      break;
    }
    ByteReader reader(bytes.data() + offset, framing::kRecordHeaderBytes);
    const std::byte* magic = reader.raw(8);
    if (magic == nullptr || !std::equal(magic, magic + 8, kRecordMagicBytes.begin())) {
      outcome = RecoveryOutcome::kCorruptTail;
      detail = "journal record magic mismatch";
      break;
    }
    const std::uint16_t version = reader.u16();
    const std::uint16_t raw_type = reader.u16();
    (void)reader.u32();  // flags
    const std::uint64_t seq = reader.u64();
    (void)reader.u64();  // tick
    const std::uint32_t payload_len = reader.u32();
    const std::uint32_t header_crc = reader.u32();
    (void)reader.u32();  // payload crc
    if (!reader.ok()) {
      outcome = RecoveryOutcome::kTruncatedTail;
      detail = "journal record header is incomplete";
      break;
    }
    const std::uint32_t computed_header_crc = Crc32c::compute(
        std::span<const std::byte>(bytes.data() + offset, framing::kRecordHeaderCrcCoverage));
    if (computed_header_crc != header_crc) {
      outcome = RecoveryOutcome::kCorruptTail;
      detail = "journal record header checksum mismatch";
      break;
    }
    if (version != kStateFormatVersion) {
      outcome = RecoveryOutcome::kVersionMismatch;
      detail = "journal record format version is not supported";
      break;
    }
    if (!is_valid_record_type(raw_type)) {
      outcome = RecoveryOutcome::kCorruptTail;
      detail = "journal record type is not recognised";
      break;
    }
    if (payload_len > limits::kMaxJournalRecordBytes) {
      outcome = RecoveryOutcome::kOversized;
      detail = "journal record payload exceeds the bound";
      break;
    }
    if (RecordSeq::from_raw(seq).raw() != seq) {
      outcome = RecoveryOutcome::kCorruptTail;
      detail = "journal record sequence is not representable";
      break;
    }
    const std::size_t total = framing::kRecordHeaderBytes + payload_len;
    if (bytes.size() - offset < total) {
      outcome = RecoveryOutcome::kTruncatedTail;
      detail = "journal ends inside a record payload";
      break;
    }
    offset += total;
    valid_end = offset;
  }
  return Status{};
}

Status DurableStore::load_snapshot(RecoveryReport& report,
                                   std::vector<std::byte>& payload) const {
  std::vector<std::byte> bytes;
  bool present = false;
  const Status read = read_file(snapshot_path(), limits::kMaxSnapshotBytes, bytes, present);
  if (!read.ok()) {
    return read;
  }
  report.snapshot_present = present;
  if (!present) {
    return Status{};
  }
  if (bytes.size() < framing::kSnapshotHeaderBytes + framing::kSnapshotTrailerBytes) {
    report.outcome = RecoveryOutcome::kTruncatedTail;
    report.detail = "snapshot is shorter than its header";
    return Status{};
  }
  ByteReader reader(bytes.data(), framing::kSnapshotHeaderBytes);
  const std::byte* magic = reader.raw(8);
  if (magic == nullptr || !std::equal(magic, magic + 8, kSnapshotMagicBytes.begin())) {
    report.outcome = RecoveryOutcome::kCorruptTail;
    report.detail = "snapshot magic mismatch";
    return Status{};
  }
  const std::uint16_t version = reader.u16();
  (void)reader.u16();
  const std::uint32_t payload_len = reader.u32();
  const std::uint64_t covered = reader.u64();
  (void)reader.u64();
  const std::uint32_t header_crc = reader.u32();
  const std::uint32_t payload_crc = reader.u32();
  if (version != kStateFormatVersion) {
    report.outcome = RecoveryOutcome::kVersionMismatch;
    report.detail = "snapshot format version is not supported";
    return Status{};
  }
  const std::uint32_t computed_header_crc = Crc32c::compute(
      std::span<const std::byte>(bytes.data(), framing::kSnapshotHeaderCrcCoverage));
  if (computed_header_crc != header_crc) {
    report.outcome = RecoveryOutcome::kCorruptTail;
    report.detail = "snapshot header checksum mismatch";
    return Status{};
  }
  const std::uint64_t expected = static_cast<std::uint64_t>(framing::kSnapshotHeaderBytes) +
                                 payload_len + framing::kSnapshotTrailerBytes;
  if (bytes.size() != expected) {
    report.outcome = RecoveryOutcome::kCorruptTail;
    report.detail = "snapshot length does not match its header";
    return Status{};
  }
  const std::span<const std::byte> body(bytes.data() + framing::kSnapshotHeaderBytes, payload_len);
  if (Crc32c::compute(body) != payload_crc) {
    report.outcome = RecoveryOutcome::kCorruptTail;
    report.detail = "snapshot payload checksum mismatch";
    return Status{};
  }
  ByteReader trailer(bytes.data() + framing::kSnapshotHeaderBytes + payload_len,
                     framing::kSnapshotTrailerBytes);
  const std::uint32_t file_crc = trailer.u32();
  if (Crc32c::compute(std::span<const std::byte>(bytes.data(), bytes.size() - 4)) != file_crc) {
    report.outcome = RecoveryOutcome::kCorruptTail;
    report.detail = "snapshot file checksum mismatch";
    return Status{};
  }
  payload.assign(body.begin(), body.end());
  report.snapshot_covered_seq = covered;
  return Status{};
}

Status DurableStore::verify(RecoveryReport& report) const {
  std::vector<std::byte> snapshot_payload;
  std::vector<std::byte> journal_bytes_image;
  RecoveryReport local{};
  const Status snapshot_status = load_snapshot(local, snapshot_payload);
  if (!snapshot_status.ok()) {
    return snapshot_status;
  }
  bool present = false;
  const Status read =
      read_file(journal_path(), limits::kMaxJournalBytes, journal_bytes_image, present);
  if (!read.ok()) {
    return read;
  }
  if (!present && !local.snapshot_present) {
    local.outcome = RecoveryOutcome::kMissing;
    local.detail = "no durable state present";
  } else if (!journal_bytes_image.empty()) {
    std::uint64_t valid_end = 0;
    RecoveryOutcome outcome = RecoveryOutcome::kClean;
    std::string detail;
    std::lock_guard<std::mutex> guard(mutex_);
    const Status scan = scan_journal(journal_bytes_image, valid_end, outcome, detail);
    if (!scan.ok()) {
      return scan;
    }
    if (outcome != RecoveryOutcome::kClean) {
      local.outcome = outcome;
      local.detail = detail;
    }
    local.bytes_valid = valid_end;
    local.bytes_discarded = journal_bytes_image.size() - valid_end;
  }
  local.format_version = kStateFormatVersion;
  report = local;
  return Status{};
}

Status DurableStore::replay(const std::function<Status(const JournaledRecord&)>& visitor,
                            RecoveryReport& report, std::vector<std::byte>& snapshot_payload) {
  RecoveryReport local{};
  local.format_version = kStateFormatVersion;
  const Status snapshot_status = load_snapshot(local, snapshot_payload);
  if (!snapshot_status.ok()) {
    return snapshot_status;
  }

  std::vector<std::byte> image;
  bool present = false;
  const Status read = read_file(journal_path(), limits::kMaxJournalBytes, image, present);
  if (!read.ok()) {
    return read;
  }
  if (!present && !local.snapshot_present) {
    local.outcome = RecoveryOutcome::kMissing;
    local.detail = "no durable state present";
    report = local;
    return Status{};
  }
  if (image.empty()) {
    if (local.snapshot_present) {
      local.outcome = RecoveryOutcome::kClean;
    }
    report = local;
    return Status{};
  }

  std::uint64_t valid_end = 0;
  RecoveryOutcome outcome = RecoveryOutcome::kClean;
  std::string detail;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const Status scan = scan_journal(image, valid_end, outcome, detail);
    if (!scan.ok()) {
      return scan;
    }
  }
  // A present, fully verified journal is a clean recovery even when no snapshot exists.
  local.outcome = outcome;
  local.detail = detail;
  local.bytes_valid = valid_end;
  local.bytes_discarded = image.size() - valid_end;

  // Parse and dispatch outside the store lock so a visitor can never deadlock against the store.
  std::size_t offset = 0;
  while (offset < valid_end) {
    ByteReader reader(image.data() + offset, framing::kRecordHeaderBytes);
    (void)reader.raw(8);
    (void)reader.u16();
    const std::uint16_t raw_type = reader.u16();
    (void)reader.u32();
    const std::uint64_t seq = reader.u64();
    const std::uint64_t tick = reader.u64();
    const std::uint32_t payload_len = reader.u32();
    (void)reader.u32();
    (void)reader.u32();
    const std::size_t total = framing::kRecordHeaderBytes + payload_len;
    JournaledRecord record;
    record.type = static_cast<RecordType>(raw_type);
    record.seq = RecordSeq::from_raw(seq);
    record.tick = Tick::from_raw(tick);
    record.payload.assign(image.begin() + static_cast<std::ptrdiff_t>(offset + framing::kRecordHeaderBytes),
                          image.begin() + static_cast<std::ptrdiff_t>(offset + total));
    const Status visited = visitor(record);
    if (!visited.ok()) {
      return visited;
    }
    local.records_replayed += 1;
    offset += total;
  }

  // Retire the unverifiable tail so the next append starts from a consistent boundary.
  if (local.bytes_discarded != 0) {
    const Status truncated = truncate_journal(valid_end);
    if (!truncated.ok()) {
      return truncated;
    }
  }
  report = local;
  return Status{};
}

}  // namespace mbg::persist
