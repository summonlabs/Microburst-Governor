// Microburst Governor - framed wire protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mbg/core/bytes.hpp"
#include "mbg/core/status.hpp"
#include "mbg/limits.hpp"
#include "mbg/model/ids.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/model/tick.hpp"
#include "mbg/version.hpp"

namespace mbg::transport {

/// Message kinds carried by a frame. Values are wire contract, so they are never renumbered.
enum class MessageType : std::uint16_t {
  kHello = 1,
  kHelloAck = 2,
  kHeartbeat = 3,
  kEvidence = 4,
  kEvidenceAck = 5,
  kFence = 6,
  kGoodbye = 7,
  kError = 8,
};

[[nodiscard]] const char* to_string(MessageType type) noexcept;
[[nodiscard]] bool is_valid_message_type(std::uint16_t raw) noexcept;

/// Fixed frame header: big endian, checksum protected, self describing.
///
/// A receiver validates the magic, the protocol version, the declared length bound and both
/// checksums before it will look at a payload byte. Malformed, truncated, oversized and
/// contradictory frames are rejected with a specific status, never repaired.
inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kFrameMagic = 0x4D424746U;  // MBGF

struct FrameHeader {
  std::uint16_t version{kWireProtocolVersion};
  MessageType type{MessageType::kHello};
  std::uint32_t flags{0};
  FrameSeq seq{};
  std::uint32_t payload_length{0};
};

struct Frame {
  FrameHeader header{};
  std::vector<std::byte> payload{};
};

/// Encodes a frame. The declared payload length is stamped from the payload itself, so a frame
/// image can never disagree with the bytes it carries.
[[nodiscard]] Status encode_frame(const Frame& frame, std::vector<std::byte>& out);

/// Validates the header image on its own, without the payload present.
[[nodiscard]] Status decode_header(std::span<const std::byte> header_bytes, FrameHeader& header);

/// Validates a complete frame image (header and payload).
[[nodiscard]] Status decode_frame(std::span<const std::byte> bytes, Frame& frame);

// --- payloads ---------------------------------------------------------------

struct HelloRequest {
  std::uint16_t protocol{kWireProtocolVersion};
  WorkerId worker{};
  IncarnationId incarnation{};
  BootId boot{};
  Epoch epoch{};
  Tick first_tick{};
};

enum class HandshakeStatus : std::uint8_t {
  kAccepted = 0,
  kStaleEpoch = 1,
  kStaleIncarnation = 2,
  kVersionMismatch = 3,
  kCapacityExceeded = 4,
  kMalformed = 5,
  kFenced = 6,
  kStaleBoot = 7,
};

[[nodiscard]] const char* to_string(HandshakeStatus status) noexcept;

struct HelloResponse {
  HandshakeStatus status{HandshakeStatus::kMalformed};
  Epoch epoch{};
  IncarnationId coordinator{};
  BootId boot{};
  std::uint32_t max_payload_bytes{limits::kMaxFramePayloadBytes};
};

struct Heartbeat {
  WorkerId worker{};
  Tick tick{};
  Epoch epoch{};
  std::uint64_t frames_sent{0};
};

struct EvidenceBatch {
  WorkerId worker{};
  Epoch epoch{};
  std::uint32_t sample_count{0};
  std::vector<Sample> samples{};
};

struct EvidenceAck {
  FrameSeq seq{};
  std::uint32_t accepted{0};
  std::uint32_t rejected{0};
  std::uint8_t classification{0};
  Epoch epoch{};
};

struct FenceNotice {
  Epoch epoch{};
  IncarnationId coordinator{};
  AttemptId attempt{};
};

void encode(const HelloRequest& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, HelloRequest& message);
void encode(const HelloResponse& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, HelloResponse& message);
void encode(const Heartbeat& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, Heartbeat& message);
void encode(const EvidenceBatch& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, EvidenceBatch& message);
void encode(const EvidenceAck& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, EvidenceAck& message);
void encode(const FenceNotice& message, ByteWriter writer);
[[nodiscard]] Status decode(ByteReader& reader, FenceNotice& message);

void encode_sample(const Sample& sample, ByteWriter writer);
[[nodiscard]] Status decode_sample(ByteReader& reader, Sample& sample);

/// Renders a frame header for diagnostics.
[[nodiscard]] std::string render_frame(const Frame& frame);

}  // namespace mbg::transport
