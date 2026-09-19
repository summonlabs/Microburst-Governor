// Microburst Governor - framed wire protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/transport/frame.hpp"

#include "mbg/core/checked.hpp"
#include "mbg/core/crc32c.hpp"

namespace mbg::transport {
namespace {

/// Bytes covered by the header checksum: everything before the two checksum words.
constexpr std::size_t kHeaderCrcCoverage = kFrameHeaderBytes - 8;

/// Builds the complete fixed header: 24 covered bytes, a header checksum and the payload checksum
/// so that a receiver can validate framing before it reads a single payload byte.
void build_header(const FrameHeader& header, std::uint32_t payload_crc,
                  std::vector<std::byte>& out) {
  out.clear();
  out.reserve(kFrameHeaderBytes);
  ByteWriter writer(out);
  writer.u32(kFrameMagic);
  writer.u16(header.version);
  writer.u16(static_cast<std::uint16_t>(header.type));
  writer.u32(header.flags);
  writer.u64(header.seq.raw());
  writer.u32(header.payload_length);
  writer.u32(Crc32c::compute(std::span<const std::byte>(out.data(), out.size())));
  writer.u32(payload_crc);
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::kHello: return "hello";
    case MessageType::kHelloAck: return "hello-ack";
    case MessageType::kHeartbeat: return "heartbeat";
    case MessageType::kEvidence: return "evidence";
    case MessageType::kEvidenceAck: return "evidence-ack";
    case MessageType::kFence: return "fence";
    case MessageType::kGoodbye: return "goodbye";
    case MessageType::kError: return "error";
  }
  return "unknown";
}

bool is_valid_message_type(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(MessageType::kHello) &&
         raw <= static_cast<std::uint16_t>(MessageType::kError);
}

const char* to_string(HandshakeStatus status) noexcept {
  switch (status) {
    case HandshakeStatus::kAccepted: return "accepted";
    case HandshakeStatus::kStaleEpoch: return "stale-epoch";
    case HandshakeStatus::kStaleIncarnation: return "stale-incarnation";
    case HandshakeStatus::kVersionMismatch: return "version-mismatch";
    case HandshakeStatus::kCapacityExceeded: return "capacity-exceeded";
    case HandshakeStatus::kMalformed: return "malformed";
    case HandshakeStatus::kFenced: return "fenced";
    case HandshakeStatus::kStaleBoot: return "stale-boot";
  }
  return "unknown";
}

Status encode_frame(const Frame& frame, std::vector<std::byte>& out) {
  if (frame.payload.size() > limits::kMaxFramePayloadBytes) {
    return Status::failure(StatusCode::kOversized, "frame payload exceeds the protocol bound");
  }
  if (!is_valid_message_type(static_cast<std::uint16_t>(frame.header.type))) {
    return Status::failure(StatusCode::kInvalidArgument, "frame message type is not defined");
  }
  if (frame.header.version != kWireProtocolVersion) {
    return Status::failure(StatusCode::kUnsupported, "frame protocol version is not supported");
  }
  // The payload is authoritative for its own length. Stamping it here removes an entire class of
  // defect in which a caller builds a frame with a stale or absent length field.
  FrameHeader header_fields = frame.header;
  header_fields.payload_length = static_cast<std::uint32_t>(frame.payload.size());
  std::vector<std::byte> header;
  build_header(header_fields, Crc32c::compute(frame.payload), header);
  out.clear();
  out.reserve(kFrameHeaderBytes + frame.payload.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return Status{};
}

Status decode_header(std::span<const std::byte> header_bytes, FrameHeader& header) {
  if (header_bytes.size() != kFrameHeaderBytes) {
    return Status::failure(StatusCode::kTruncated, "frame header is not a full header");
  }
  ByteReader reader(header_bytes);
  const std::uint32_t magic = reader.u32();
  if (magic != kFrameMagic) {
    return Status::failure(StatusCode::kCorrupt, "frame magic mismatch");
  }
  const std::uint16_t version = reader.u16();
  if (version != kWireProtocolVersion) {
    return Status::failure(StatusCode::kUnsupported, "frame protocol version is not supported");
  }
  const std::uint16_t raw_type = reader.u16();
  if (!is_valid_message_type(raw_type)) {
    return Status::failure(StatusCode::kCorrupt, "frame message type is not defined");
  }
  const std::uint32_t flags = reader.u32();
  const std::uint64_t seq = reader.u64();
  const std::uint32_t payload_length = reader.u32();
  const std::uint32_t header_crc = reader.u32();
  const std::uint32_t payload_crc = reader.u32();
  static_cast<void>(payload_crc);
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "frame header is truncated");
  }
  if (Crc32c::compute(header_bytes.first(kHeaderCrcCoverage)) != header_crc) {
    return Status::failure(StatusCode::kCorrupt, "frame header checksum mismatch");
  }
  if (payload_length > limits::kMaxFramePayloadBytes) {
    return Status::failure(StatusCode::kOversized, "frame payload length exceeds the bound");
  }
  header.version = version;
  header.type = static_cast<MessageType>(raw_type);
  header.flags = flags;
  header.seq = FrameSeq::from_raw(seq);
  header.payload_length = payload_length;
  return Status{};
}

Status decode_frame(std::span<const std::byte> bytes, Frame& frame) {
  if (bytes.size() < kFrameHeaderBytes) {
    return Status::failure(StatusCode::kTruncated, "frame is shorter than its header");
  }
  FrameHeader header;
  const Status decoded = decode_header(bytes.first(kFrameHeaderBytes), header);
  if (!decoded.ok()) {
    return decoded;
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(kFrameHeaderBytes) + static_cast<std::uint64_t>(header.payload_length);
  if (bytes.size() < expected) {
    return Status::failure(StatusCode::kTruncated, "frame payload is incomplete");
  }
  if (bytes.size() > expected) {
    return Status::failure(StatusCode::kCorrupt, "frame carries trailing bytes");
  }
  ByteReader trailer(bytes.data() + kFrameHeaderBytes - 4, 4);
  const std::uint32_t payload_crc = trailer.u32();
  const auto payload = bytes.subspan(kFrameHeaderBytes, header.payload_length);
  if (Crc32c::compute(payload) != payload_crc) {
    return Status::failure(StatusCode::kCorrupt, "frame payload checksum mismatch");
  }
  frame.header = header;
  frame.payload.assign(payload.begin(), payload.end());
  return Status{};
}

void encode(const HelloRequest& message, ByteWriter writer) {
  writer.u16(message.protocol);
  writer.u64(message.worker.raw());
  writer.u64(message.incarnation.raw());
  writer.u64(message.boot.raw());
  writer.u32(message.epoch.raw());
  writer.u64(message.first_tick.value());
}

Status decode(ByteReader& reader, HelloRequest& message) {
  message.protocol = reader.u16();
  message.worker = WorkerId::from_raw(reader.u64());
  message.incarnation = IncarnationId::from_raw(reader.u64());
  message.boot = BootId::from_raw(reader.u64());
  message.epoch = Epoch::from_raw(reader.u32());
  message.first_tick = Tick::from_raw(reader.u64());
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "hello payload is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "hello payload carries trailing bytes");
  }
  if (!message.worker.valid()) {
    return Status::failure(StatusCode::kInvalidArgument, "hello carries no worker identity");
  }
  return Status{};
}

void encode(const HelloResponse& message, ByteWriter writer) {
  writer.u8(static_cast<std::uint8_t>(message.status));
  writer.u32(message.epoch.raw());
  writer.u64(message.coordinator.raw());
  writer.u64(message.boot.raw());
  writer.u32(message.max_payload_bytes);
}

Status decode(ByteReader& reader, HelloResponse& message) {
  message.status = static_cast<HandshakeStatus>(reader.u8());
  message.epoch = Epoch::from_raw(reader.u32());
  message.coordinator = IncarnationId::from_raw(reader.u64());
  message.boot = BootId::from_raw(reader.u64());
  message.max_payload_bytes = reader.u32();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "hello response payload is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "hello response payload carries trailing bytes");
  }
  if (static_cast<std::uint8_t>(message.status) >
      static_cast<std::uint8_t>(HandshakeStatus::kStaleBoot)) {
    return Status::failure(StatusCode::kCorrupt, "handshake status is out of range");
  }
  return Status{};
}

void encode(const Heartbeat& message, ByteWriter writer) {
  writer.u64(message.worker.raw());
  writer.u64(message.tick.value());
  writer.u32(message.epoch.raw());
  writer.u64(message.frames_sent);
}

Status decode(ByteReader& reader, Heartbeat& message) {
  message.worker = WorkerId::from_raw(reader.u64());
  message.tick = Tick::from_raw(reader.u64());
  message.epoch = Epoch::from_raw(reader.u32());
  message.frames_sent = reader.u64();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "heartbeat payload is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "heartbeat payload carries trailing bytes");
  }
  return Status{};
}

void encode(const EvidenceBatch& message, ByteWriter writer) {
  writer.u64(message.worker.raw());
  writer.u32(message.epoch.raw());
  writer.u32(message.sample_count);
  for (const Sample& sample : message.samples) {
    encode_sample(sample, writer);
  }
}

Status decode(ByteReader& reader, EvidenceBatch& message) {
  message.worker = WorkerId::from_raw(reader.u64());
  message.epoch = Epoch::from_raw(reader.u32());
  message.sample_count = reader.u32();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "evidence payload is truncated");
  }
  constexpr std::uint32_t kMaxSamplesPerFrame = limits::kMaxFramePayloadBytes / 64U;
  if (message.sample_count > kMaxSamplesPerFrame) {
    return Status::failure(StatusCode::kOversized, "evidence batch exceeds the sample bound");
  }
  message.samples.clear();
  message.samples.reserve(message.sample_count);
  for (std::uint32_t i = 0; i < message.sample_count; ++i) {
    Sample sample;
    const Status status = decode_sample(reader, sample);
    if (!status.ok()) {
      return status;
    }
    message.samples.push_back(sample);
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "evidence payload carries trailing bytes");
  }
  return Status{};
}

void encode(const EvidenceAck& message, ByteWriter writer) {
  writer.u64(message.seq.raw());
  writer.u32(message.accepted);
  writer.u32(message.rejected);
  writer.u8(message.classification);
  writer.u32(message.epoch.raw());
}

Status decode(ByteReader& reader, EvidenceAck& message) {
  message.seq = FrameSeq::from_raw(reader.u64());
  message.accepted = reader.u32();
  message.rejected = reader.u32();
  message.classification = reader.u8();
  message.epoch = Epoch::from_raw(reader.u32());
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "evidence ack payload is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "evidence ack payload carries trailing bytes");
  }
  return Status{};
}

void encode(const FenceNotice& message, ByteWriter writer) {
  writer.u32(message.epoch.raw());
  writer.u64(message.coordinator.raw());
  writer.u64(message.attempt.raw());
}

Status decode(ByteReader& reader, FenceNotice& message) {
  message.epoch = Epoch::from_raw(reader.u32());
  message.coordinator = IncarnationId::from_raw(reader.u64());
  message.attempt = AttemptId::from_raw(reader.u64());
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "fence payload is truncated");
  }
  if (!reader.at_end()) {
    return Status::failure(StatusCode::kCorrupt, "fence payload carries trailing bytes");
  }
  return Status{};
}

void encode_sample(const Sample& sample, ByteWriter writer) {
  writer.u64(sample.stream.resource.raw());
  writer.u64(sample.stream.queue.raw());
  writer.u64(sample.stream.path.valid() ? sample.stream.path.raw() : PathId::kInvalid);
  writer.u64(sample.tick.value());
  writer.u64(sample.seq.raw());
  writer.u32(sample.resource_generation.raw());
  writer.u64(sample.depth);
  writer.u64(sample.occupancy_bytes);
  writer.u64(sample.capacity_bytes);
  writer.i64(sample.ingress_rate_q16);
  writer.i64(sample.egress_rate_q16);
  writer.u64(sample.ingress_total);
  writer.u64(sample.egress_total);
  writer.u64(sample.drop_total);
  writer.u64(sample.mark_total);
  writer.u32(static_cast<std::uint32_t>(sample.flags));
  writer.u64(sample.provenance.valid() ? sample.provenance.raw() : ProvenanceId::kInvalid);
  writer.u8(sample.affected_class_count);
  for (std::uint8_t i = 0; i < sample.affected_class_count; ++i) {
    writer.u64(sample.affected_classes[i].raw());
  }
}

Status decode_sample(ByteReader& reader, Sample& sample) {
  sample.stream.resource = ResourceId::from_raw(reader.u64());
  sample.stream.queue = QueueId::from_raw(reader.u64());
  sample.stream.path = PathId::from_raw(reader.u64());
  sample.tick = Tick::from_raw(reader.u64());
  sample.seq = SampleSeq::from_raw(reader.u64());
  sample.resource_generation = ResourceGeneration::from_raw(reader.u32());
  sample.depth = reader.u64();
  sample.occupancy_bytes = reader.u64();
  sample.capacity_bytes = reader.u64();
  sample.ingress_rate_q16 = reader.i64();
  sample.egress_rate_q16 = reader.i64();
  sample.ingress_total = reader.u64();
  sample.egress_total = reader.u64();
  sample.drop_total = reader.u64();
  sample.mark_total = reader.u64();
  const std::uint32_t flags = reader.u32();
  const std::uint64_t provenance = reader.u64();
  sample.provenance = provenance == ProvenanceId::kInvalid ? ProvenanceId{} : ProvenanceId::from_raw(provenance);
  sample.affected_class_count = reader.u8();
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "sample payload is truncated");
  }
  if (sample.affected_class_count > sample.affected_classes.size()) {
    return Status::failure(StatusCode::kCorrupt, "sample class count exceeds the fixed capacity");
  }
  constexpr std::uint32_t kKnownFlags = 0x1FU;
  if ((flags & ~kKnownFlags) != 0U) {
    return Status::failure(StatusCode::kCorrupt, "sample carries undefined flag bits");
  }
  sample.flags = static_cast<SampleFlag>(flags);
  for (std::uint8_t i = 0; i < sample.affected_class_count; ++i) {
    sample.affected_classes[i] = ClassId::from_raw(reader.u64());
  }
  if (!reader.ok()) {
    return Status::failure(StatusCode::kTruncated, "sample class list is truncated");
  }
  return validate_sample(sample);
}

std::string render_frame(const Frame& frame) {
  std::string out;
  out.reserve(128);
  out.append("frame type=");
  out.append(to_string(frame.header.type));
  out.append(" seq=");
  out.append(std::to_string(frame.header.seq.raw()));
  out.append(" bytes=");
  out.append(std::to_string(frame.payload.size()));
  return out;
}

}  // namespace mbg::transport
