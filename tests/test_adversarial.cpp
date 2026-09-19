// Microburst Governor - adversarial and malformed input tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <string>
#include <vector>

#include "mbg/model/event.hpp"
#include "mbg/model/intervention.hpp"
#include "mbg/model/policy.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/persist/snapshot.hpp"
#include "mbg/transport/frame.hpp"
#include "mbg/authority/fence.hpp"
#include "mbg/governor.hpp"
#include "mbg/synthetic/generator.hpp"
#include "test_framework.hpp"

using namespace mbg;

namespace {

std::vector<std::byte> framed(transport::MessageType type, const std::vector<std::byte>& payload) {
  transport::Frame frame;
  frame.header.type = type;
  frame.header.seq = FrameSeq::from_raw(1);
  frame.payload = payload;
  std::vector<std::byte> out;
  static_cast<void>(transport::encode_frame(frame, out));
  return out;
}

}  // namespace

MBG_TEST(adversarial, frame_decoder_rejects_structural_damage) {
  std::vector<std::byte> payload;
  ByteWriter writer(payload);
  writer.u64(0xDEADBEEFULL);
  const std::vector<std::byte> good = framed(transport::MessageType::kHeartbeat, payload);

  transport::Frame frame;
  MBG_REQUIRE(transport::decode_frame(good, frame).ok());
  MBG_CHECK(frame.header.type == transport::MessageType::kHeartbeat);

  std::vector<std::byte> damaged = good;
  damaged[0] = static_cast<std::byte>(0x00);
  MBG_CHECK(transport::decode_frame(damaged, frame).code() == StatusCode::kCorrupt);

  damaged = good;
  damaged[4] = static_cast<std::byte>(0x09);  // protocol version
  MBG_CHECK(transport::decode_frame(damaged, frame).code() == StatusCode::kUnsupported);

  damaged = good;
  damaged[6] = static_cast<std::byte>(0xFF);  // message type
  MBG_CHECK(transport::decode_frame(damaged, frame).code() == StatusCode::kCorrupt);

  // Header checksum lives at offset 24 of the fixed 32 byte header.
  damaged = good;
  damaged[24] = static_cast<std::byte>(static_cast<unsigned>(damaged[24]) ^ 0xFFU);
  MBG_CHECK(transport::decode_frame(damaged, frame).code() == StatusCode::kCorrupt);

  damaged = good;
  damaged.back() = static_cast<std::byte>(static_cast<unsigned>(damaged.back()) ^ 0x01U);
  MBG_CHECK(transport::decode_frame(damaged, frame).code() == StatusCode::kCorrupt);

  MBG_CHECK(transport::decode_frame(std::span<const std::byte>(good.data(), 8), frame).code() ==
            StatusCode::kTruncated);

  std::vector<std::byte> trailing = good;
  trailing.push_back(std::byte{0});
  MBG_CHECK(transport::decode_frame(trailing, frame).code() == StatusCode::kCorrupt);

  // An oversized declared payload never reaches allocation.
  std::vector<std::byte> oversized = good;
  const std::uint32_t huge = limits::kMaxFramePayloadBytes + 1U;
  oversized[20] = static_cast<std::byte>((huge >> 24U) & 0xFFU);
  oversized[21] = static_cast<std::byte>((huge >> 16U) & 0xFFU);
  oversized[22] = static_cast<std::byte>((huge >> 8U) & 0xFFU);
  oversized[23] = static_cast<std::byte>(huge & 0xFFU);
  MBG_CHECK(transport::decode_header(std::span<const std::byte>(oversized.data(), 32), frame.header)
                .ok() == false);
}

MBG_TEST(adversarial, frame_encoder_refuses_undefined_messages) {
  transport::Frame frame;
  frame.header.type = static_cast<transport::MessageType>(9999);
  std::vector<std::byte> out;
  MBG_CHECK(transport::encode_frame(frame, out).code() == StatusCode::kInvalidArgument);
  frame.header.type = transport::MessageType::kHeartbeat;
  frame.payload.assign(limits::kMaxFramePayloadBytes + 1U, std::byte{0});
  MBG_CHECK(transport::encode_frame(frame, out).code() == StatusCode::kOversized);
}

MBG_TEST(adversarial, payload_decoders_reject_truncation_and_trailing_bytes) {
  std::vector<std::byte> payload;
  ByteWriter writer(payload);
  transport::Heartbeat heartbeat;
  heartbeat.worker = WorkerId::from_raw(1);
  heartbeat.tick = Tick::from_raw(5);
  heartbeat.epoch = Epoch::from_raw(1);
  heartbeat.frames_sent = 3;
  transport::encode(heartbeat, writer);

  transport::Heartbeat decoded;
  ByteReader reader(payload);
  MBG_CHECK(transport::decode(reader, decoded).ok());
  MBG_CHECK(decoded.tick == Tick::from_raw(5));

  payload.push_back(std::byte{0});
  ByteReader trailing(payload);
  MBG_CHECK(transport::decode(trailing, decoded).code() == StatusCode::kCorrupt);

  ByteReader short_reader(payload.data(), 4);
  MBG_CHECK(transport::decode(short_reader, decoded).code() == StatusCode::kTruncated);
}

MBG_TEST(adversarial, evidence_batch_rejects_an_impossible_sample_count) {
  std::vector<std::byte> payload;
  ByteWriter writer(payload);
  writer.u64(1);
  writer.u32(1);
  writer.u32(0xFFFFFFU);
  ByteReader reader(payload);
  transport::EvidenceBatch batch;
  MBG_CHECK(transport::decode(reader, batch).code() == StatusCode::kOversized);
}

MBG_TEST(adversarial, policy_and_event_decoders_reject_corruption) {
  Policy policy = make_default_policy();
  std::vector<std::byte> bytes;
  ByteWriter writer(bytes);
  encode(policy, writer);
  Policy decoded;
  ByteReader reader(bytes);
  MBG_CHECK(decode(reader, decoded).ok());
  MBG_CHECK_EQ(decoded.digest(), policy.digest());

  ByteReader truncated(bytes.data(), bytes.size() / 2U);
  MBG_CHECK(!decode(truncated, decoded).ok());

  // A structurally valid encoding whose semantic content is impossible is still rejected.
  std::vector<std::byte> incoherent;
  ByteWriter incoherent_writer(incoherent);
  Policy invalid = make_default_policy();
  invalid.detection.release_depth = invalid.detection.onset_min_depth + 1U;
  encode(invalid, incoherent_writer);
  Policy rejected;
  ByteReader incoherent_reader(incoherent);
  MBG_CHECK(!decode(incoherent_reader, rejected).ok());

  BurstEvent event;
  event.id = EventId::from_raw(1);
  event.stream.resource = ResourceId::from_raw(1);
  event.stream.queue = QueueId::from_raw(1);
  std::vector<std::byte> event_bytes;
  ByteWriter event_writer(event_bytes);
  encode(event, event_writer);
  BurstEvent decoded_event;
  ByteReader event_reader(event_bytes);
  MBG_CHECK(decode(event_reader, decoded_event).ok());

  // A positive severity with unknown authority is contradictory and must not load.
  BurstEvent contradictory = event;
  contradictory.severity = SeverityClass::kSevere;
  contradictory.authority = EvidenceAuthority::kUnknown;
  std::vector<std::byte> contradictory_bytes;
  ByteWriter contradictory_writer(contradictory_bytes);
  encode(contradictory, contradictory_writer);
  BurstEvent loaded;
  ByteReader contradictory_reader(contradictory_bytes);
  MBG_CHECK(decode(contradictory_reader, loaded).code() == StatusCode::kCorrupt);
}

MBG_TEST(adversarial, persisted_state_rejects_magic_and_version_damage) {
  persist::PersistedState state;
  state.boot = BootId::from_raw(3);
  state.epoch = Epoch::from_raw(4);
  state.incarnation = IncarnationId::from_raw(5);
  state.decisions = 6;
  std::vector<std::byte> bytes;
  persist::encode(state, bytes);

  persist::PersistedState decoded;
  MBG_REQUIRE(persist::decode(bytes, decoded).ok());
  MBG_CHECK_EQ(decoded.decisions, 6U);
  MBG_CHECK(decoded.epoch == Epoch::from_raw(4));

  std::vector<std::byte> damaged = bytes;
  damaged[0] = std::byte{0};
  MBG_CHECK(persist::decode(damaged, decoded).code() == StatusCode::kCorrupt);

  damaged = bytes;
  damaged[5] = std::byte{9};
  MBG_CHECK(persist::decode(damaged, decoded).code() == StatusCode::kUnsupported);

  damaged = bytes;
  damaged.resize(bytes.size() - 1U);
  MBG_CHECK(!persist::decode(damaged, decoded).ok());

  damaged = bytes;
  damaged.push_back(std::byte{0});
  MBG_CHECK(persist::decode(damaged, decoded).code() == StatusCode::kCorrupt);
}

MBG_TEST(adversarial, durable_store_enforces_its_bounds) {
  const test::TempDir directory("bounds");
  persist::DurableStore store;
  persist::DurableStore::Config config;
  config.directory = directory.path();
  config.max_journal_bytes = 512;
  config.fsync_on_append = false;
  MBG_REQUIRE(store.open(config).ok());

  std::vector<std::byte> payload(64, std::byte{1});
  std::uint64_t sequence = 1;
  Status last{};
  for (; sequence <= 64; ++sequence) {
    last = store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(sequence),
                        Tick::from_raw(sequence), payload);
    if (!last.ok()) {
      break;
    }
  }
  MBG_CHECK(last.code() == StatusCode::kCapacityExceeded);
  MBG_CHECK(store.needs_compaction());

  std::vector<std::byte> huge(limits::kMaxJournalRecordBytes + 1U, std::byte{0});
  MBG_CHECK(store.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(1), Tick{}, huge)
                .code() == StatusCode::kOversized);

  persist::DurableStore unopened;
  MBG_CHECK(unopened.append(persist::RecordType::kCheckpoint, RecordSeq::from_raw(1), Tick{}, {})
                .code() == StatusCode::kIo);
}

MBG_TEST(adversarial, authority_claims_are_refused_component_wise) {
  AuthorityState current;
  current.epoch = Epoch::from_raw(5);
  current.incarnation = IncarnationId::from_raw(99);
  current.boot = BootId::from_raw(7);

  MBG_CHECK(validate_claim(current, current, kWireProtocolVersion, kWireProtocolVersion) ==
            ClaimVerdict::kAccepted);

  AuthorityState stale = current;
  stale.epoch = Epoch::from_raw(4);
  MBG_CHECK(validate_claim(current, stale, kWireProtocolVersion, kWireProtocolVersion) ==
            ClaimVerdict::kEpochMismatch);

  AuthorityState wrong_boot = current;
  wrong_boot.boot = BootId::from_raw(6);
  MBG_CHECK(validate_claim(current, wrong_boot, kWireProtocolVersion, kWireProtocolVersion) ==
            ClaimVerdict::kBootMismatch);

  // A different claimant incarnation is expected: a worker is never the coordinator.
  AuthorityState other_incarnation = current;
  other_incarnation.incarnation = IncarnationId::from_raw(100);
  MBG_CHECK(validate_claim(current, other_incarnation, kWireProtocolVersion, kWireProtocolVersion) ==
            ClaimVerdict::kAccepted);

  MBG_CHECK(validate_claim(current, current, 99, kWireProtocolVersion) ==
            ClaimVerdict::kVersionMismatch);

  AuthorityState unbound;
  MBG_CHECK(validate_claim(unbound, unbound, kWireProtocolVersion, kWireProtocolVersion) ==
            ClaimVerdict::kNotBound);

  AuthorityState advanced;
  MBG_REQUIRE(advance_authority(current, advanced).ok());
  MBG_CHECK(advanced.epoch == Epoch::from_raw(6));
  MBG_CHECK(advanced.boot == BootId::from_raw(8));
  AuthorityState exhausted;
  exhausted.epoch = Epoch::from_raw(~std::uint32_t{0});
  exhausted.boot = BootId::from_raw(1);
  AuthorityState ignored;
  MBG_CHECK(!advance_authority(exhausted, ignored).ok());
}

MBG_TEST(adversarial, trace_parser_rejects_hostile_lines) {
  std::vector<Sample> samples;
  const std::string oversized(limits::kMaxTraceLineBytes + 32U, '1');
  MBG_CHECK(synthetic::decode_trace(oversized, samples, 16).code() == StatusCode::kOversized);

  MBG_CHECK(!synthetic::decode_trace("1 2 3 4 5 6 7 8 9 10 11 12 13 1 15 16\n", samples, 16).ok());
  MBG_CHECK(!synthetic::decode_trace("-1 2 3 4 5 6 7 8 9 10 11 12 13 1\n", samples, 16).ok());
  MBG_CHECK(!synthetic::decode_trace("", samples, 16).ok() || samples.empty());
  MBG_CHECK(synthetic::decode_trace("\n\n\n", samples, 16).ok());
  MBG_CHECK_EQ(samples.size(), 0U);
}
