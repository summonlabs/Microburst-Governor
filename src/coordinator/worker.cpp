// Microburst Governor - worker side of the framed authority transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/coordinator/worker.hpp"

#include <utility>

namespace mbg {

Worker::~Worker() { close(); }

Status Worker::connect(const Config& config) {
  const Status started = transport::net_startup();
  if (!started.ok()) {
    return started;
  }
  if (!config.id.valid()) {
    return Status::failure(StatusCode::kInvalidArgument, "worker identity is required");
  }
  config_ = config;
  transport::Socket socket;
  const Status connected = transport::connect_loopback(config_.host.c_str(), config_.port, socket);
  if (!connected.ok()) {
    return connected;
  }
  static_cast<void>(socket.set_nodelay(true));
  channel_ = std::make_unique<transport::FramedChannel>(std::move(socket));
  sequence_ = FrameSeq::from_raw(1);
  return Status{};
}

Status Worker::handshake(transport::HelloResponse& response) {
  if (channel_ == nullptr) {
    return Status::failure(StatusCode::kIo, "worker is not connected");
  }
  transport::HelloRequest hello;
  hello.protocol = kWireProtocolVersion;
  hello.worker = config_.id;
  hello.incarnation = config_.incarnation;
  hello.boot = config_.boot;
  hello.epoch = config_.epoch;
  hello.first_tick = config_.first_tick;

  transport::Frame frame;
  frame.header.type = transport::MessageType::kHello;
  frame.header.seq = sequence_;
  transport::encode(hello, ByteWriter(frame.payload));
  const Status sent = channel_->send(frame);
  if (!sent.ok()) {
    return sent;
  }

  transport::Frame reply;
  const Status received = channel_->receive(reply);
  if (!received.ok()) {
    return received;
  }
  if (reply.header.type == transport::MessageType::kFence) {
    fenced_ = true;
    handshake_status_ = transport::HandshakeStatus::kFenced;
    return Status::failure(StatusCode::kStale, "coordinator fenced this worker");
  }
  if (reply.header.type != transport::MessageType::kHelloAck) {
    return Status::failure(StatusCode::kProtocol, "coordinator did not answer the handshake");
  }
  ByteReader reader(reply.payload);
  const Status decoded = transport::decode(reader, response);
  if (!decoded.ok()) {
    return decoded;
  }
  handshake_status_ = response.status;
  granted_epoch_ = response.epoch;
  if (response.status != transport::HandshakeStatus::kAccepted) {
    fenced_ = response.status == transport::HandshakeStatus::kStaleEpoch ||
              response.status == transport::HandshakeStatus::kFenced;
    return Status::failure(StatusCode::kStale, "coordinator refused the handshake");
  }
  return Status{};
}

Status Worker::send_heartbeat(Tick tick) {
  if (channel_ == nullptr || !channel_->valid()) {
    return Status::failure(StatusCode::kIo, "worker is not connected");
  }
  transport::Heartbeat heartbeat;
  heartbeat.worker = config_.id;
  heartbeat.tick = tick;
  heartbeat.epoch = granted_epoch_.valid() ? granted_epoch_ : config_.epoch;
  heartbeat.frames_sent = channel_->frames_sent();

  transport::Frame frame;
  frame.header.type = transport::MessageType::kHeartbeat;
  sequence_ = FrameSeq::from_raw(sequence_.raw() + 1U);
  frame.header.seq = sequence_;
  transport::encode(heartbeat, ByteWriter(frame.payload));
  return channel_->send(frame);
}

Status Worker::send_evidence(const transport::EvidenceBatch& batch, transport::EvidenceAck* ack) {
  if (channel_ == nullptr || !channel_->valid()) {
    return Status::failure(StatusCode::kIo, "worker is not connected");
  }
  transport::Frame frame;
  frame.header.type = transport::MessageType::kEvidence;
  sequence_ = FrameSeq::from_raw(sequence_.raw() + 1U);
  frame.header.seq = sequence_;
  transport::encode(batch, ByteWriter(frame.payload));
  const Status sent = channel_->send(frame);
  if (!sent.ok()) {
    return sent;
  }
  transport::Frame reply;
  const Status received = channel_->receive(reply);
  if (!received.ok()) {
    return received;
  }
  if (reply.header.type == transport::MessageType::kFence) {
    fenced_ = true;
    return Status::failure(StatusCode::kStale, "coordinator fenced this worker mid stream");
  }
  if (reply.header.type != transport::MessageType::kEvidenceAck) {
    return Status::failure(StatusCode::kProtocol, "coordinator did not acknowledge evidence");
  }
  if (ack != nullptr) {
    ByteReader reader(reply.payload);
    const Status decoded = transport::decode(reader, *ack);
    if (!decoded.ok()) {
      return decoded;
    }
  }
  return Status{};
}

Status Worker::say_goodbye() {
  if (channel_ == nullptr || !channel_->valid()) {
    return Status{};
  }
  transport::Frame frame;
  frame.header.type = transport::MessageType::kGoodbye;
  sequence_ = FrameSeq::from_raw(sequence_.raw() + 1U);
  frame.header.seq = sequence_;
  ByteWriter writer(frame.payload);
  writer.u64(config_.id.raw());
  const Status sent = channel_->send(frame);
  channel_->close();
  return sent;
}

void Worker::close() {
  if (channel_ != nullptr) {
    channel_->close();
    channel_.reset();
  }
}

}  // namespace mbg
