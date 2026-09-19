// Microburst Governor - coordinator authority.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/coordinator/coordinator.hpp"

#include <algorithm>
#include <utility>

#include "mbg/authority/fence.hpp"
#include "mbg/core/hash.hpp"
#include "mbg/platform/process.hpp"

namespace mbg {
namespace {

constexpr std::size_t kFrameSeqStart = 1;

void encode_error(ByteWriter& writer, StatusCode code, const char* detail) {
  writer.u16(static_cast<std::uint16_t>(code));
  writer.text(detail == nullptr ? "" : detail);
}

}  // namespace

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() { stop(); }

Status Coordinator::recover_durable_authority(persist::DurableStore& store, Epoch& epoch,
                                              BootId& boot, bool& present) {
  epoch = Epoch::from_raw(0);
  boot = BootId::from_raw(0);
  present = false;
  persist::RecoveryReport report;
  std::vector<std::byte> snapshot_payload;
  std::vector<std::pair<Epoch, BootId>> observed;

  const Status replayed = store.replay(
      [&observed](const persist::JournaledRecord& record) -> Status {
        if (record.type != persist::RecordType::kAuthorityBound) {
          return Status{};
        }
        ByteReader reader(record.payload);
        const Epoch record_epoch = Epoch::from_raw(reader.u32());
        (void)reader.u64();
        const BootId record_boot = BootId::from_raw(reader.u64());
        if (!reader.ok() || !reader.at_end()) {
          return Status::failure(StatusCode::kCorrupt, "authority record payload is malformed");
        }
        observed.emplace_back(record_epoch, record_boot);
        return Status{};
      },
      report, snapshot_payload);

  if (report.snapshot_present && !snapshot_payload.empty()) {
    persist::PersistedState state;
    if (persist::decode(snapshot_payload, state).ok()) {
      observed.emplace_back(state.epoch, state.boot);
    }
  }
  if (!replayed.ok()) {
    return replayed;
  }
  for (const auto& [record_epoch, record_boot] : observed) {
    if (record_epoch.raw() >= epoch.raw()) {
      epoch = record_epoch;
    }
    if (record_boot.raw() >= boot.raw()) {
      boot = record_boot;
    }
  }
  present = !observed.empty();
  return Status{};
}

Status Coordinator::start(const Config& config) {
  if (config.governor == nullptr) {
    return Status::failure(StatusCode::kInvalidArgument, "coordinator requires a governor");
  }
  if (config.max_workers == 0 || config.max_workers > limits::kMaxConnections) {
    return Status::failure(StatusCode::kOutOfRange, "worker bound outside supported range");
  }
  const Status started = transport::net_startup();
  if (!started.ok()) {
    return started;
  }

  config_ = config;
  incarnation_ = config.incarnation.valid() ? config.incarnation : platform::mint_incarnation();
  epoch_ = config.seed_epoch;
  boot_ = config.seed_boot;
  authority_recovered_ = false;
  previous_epoch_ = Epoch::from_raw(0);

  if (config_.store != nullptr && config_.recover_authority) {
    Epoch durable_epoch{};
    BootId durable_boot{};
    bool present = false;
    const Status recovered =
        recover_durable_authority(*config_.store, durable_epoch, durable_boot, present);
    if (!recovered.ok()) {
      return recovered;
    }
    if (present) {
      previous_epoch_ = durable_epoch;
      AuthorityState previous;
      previous.epoch = durable_epoch;
      previous.boot = durable_boot;
      previous.incarnation = incarnation_;
      AuthorityState advanced;
      const Status status = advance_authority(previous, advanced);
      if (!status.ok()) {
        return status;
      }
      epoch_ = advanced.epoch;
      boot_ = advanced.boot;
      authority_recovered_ = true;
    }
  }
  if (epoch_.raw() == 0) {
    epoch_ = Epoch::from_raw(1);
  }

  const Status bound = config_.governor->bind_authority(epoch_, incarnation_, boot_);
  if (!bound.ok()) {
    return bound;
  }

  const Status listening = listener_.bind_loopback(config_.port);
  if (!listening.ok()) {
    return listening;
  }
  port_ = listener_.port();
  running_.store(true);
  started_.store(true);
  accept_thread_ = std::thread([this]() { run_accept_loop(); });
  return Status{};
}

void Coordinator::stop() {
  if (!started_.load()) {
    return;
  }
  running_.store(false);
  listener_.interrupt();
  listener_.close();
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  std::vector<std::shared_ptr<transport::FramedChannel>> channels;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    channels = session_channels_;
  }
  for (const auto& channel : channels) {
    if (channel != nullptr) {
      channel->interrupt();
    }
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    sessions_cv_.wait(lock, [this]() { return active_sessions_ == 0; });
    session_channels_.clear();
  }
  started_.store(false);
}

bool Coordinator::wait_for_sessions() {
  if (!started_.load()) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  sessions_cv_.wait(lock, [this]() { return active_sessions_ == 0; });
  return true;
}

void Coordinator::run_accept_loop() {
  while (running_.load()) {
    transport::Socket socket;
    const Status accepted = listener_.accept(socket);
    if (!accepted.ok()) {
      break;  // the listener was interrupted or closed, which is the shutdown path
    }
    if (!running_.load()) {
      socket.close();
      break;
    }
    counters_.connections_accepted += 1;
    auto channel = std::make_shared<transport::FramedChannel>(std::move(socket));
    static_cast<void>(channel->socket().set_nodelay(true));
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (active_sessions_ >= config_.max_workers) {
        counters_.capacity_rejections += 1;
        continue;  // the channel leaves scope and closes
      }
    }
    register_session(channel);
    std::thread([this, channel]() { run_session(channel); }).detach();
  }
}

void Coordinator::register_session(const std::shared_ptr<transport::FramedChannel>& channel) {
  std::lock_guard<std::mutex> guard(mutex_);
  active_sessions_ += 1;
  session_channels_.push_back(channel);
}

void Coordinator::retire_session(const std::shared_ptr<transport::FramedChannel>& channel) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (active_sessions_ > 0) {
    active_sessions_ -= 1;
  }
  const auto found = std::find(session_channels_.begin(), session_channels_.end(), channel);
  if (found != session_channels_.end()) {
    session_channels_.erase(found);
  }
  sessions_cv_.notify_all();
}

Status Coordinator::send_error(const std::shared_ptr<transport::FramedChannel>& channel,
                               StatusCode code, const char* detail) {
  transport::Frame frame;
  frame.header.type = transport::MessageType::kError;
  frame.header.seq = FrameSeq::from_raw(kFrameSeqStart);
  ByteWriter writer(frame.payload);
  encode_error(writer, code, detail);
  counters_.protocol_errors += 1;
  return channel->send(frame);
}

void Coordinator::run_session(const std::shared_ptr<transport::FramedChannel>& channel) {
  WorkerId worker{};
  bool registered = false;

  auto finish = [&](bool graceful) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (registered) {
        auto found = workers_.find(worker);
        if (found != workers_.end()) {
          found->second.connected = false;
          found->second.fenced = !graceful;
          if (!graceful) {
            found->second.fence_attempt = AttemptId::from_raw(
                hash_combine(worker.raw(), found->second.frames + 1U));
            counters_.workers_fenced += 1;
          }
        }
        counters_.worker_disconnects += 1;
      }
    }
    channel->close();
    retire_session(channel);
  };

  transport::Frame frame;
  Status received = channel->receive(frame);
  if (!received.ok() || frame.header.type != transport::MessageType::kHello) {
    counters_.handshakes_rejected += 1;
    finish(false);
    return;
  }
  transport::HelloRequest hello;
  ByteReader hello_reader(frame.payload);
  if (!transport::decode(hello_reader, hello).ok()) {
    counters_.handshakes_rejected += 1;
    finish(false);
    return;
  }

  transport::HelloResponse response;
  response.epoch = epoch_;
  response.coordinator = incarnation_;
  response.boot = boot_;
  response.max_payload_bytes = limits::kMaxFramePayloadBytes;

  AuthorityState current;
  current.epoch = epoch_;
  current.incarnation = incarnation_;
  current.boot = boot_;
  AuthorityState claim;
  claim.epoch = hello.epoch;
  claim.incarnation = hello.incarnation;
  claim.boot = hello.boot;
  const ClaimVerdict verdict =
      validate_claim(current, claim, hello.protocol, kWireProtocolVersion);

  if (verdict == ClaimVerdict::kVersionMismatch) {
    response.status = transport::HandshakeStatus::kVersionMismatch;
    counters_.version_rejections += 1;
    counters_.handshakes_rejected += 1;
  } else if (verdict == ClaimVerdict::kEpochMismatch) {
    // A worker from a previous epoch is fenced, never admitted.
    response.status = transport::HandshakeStatus::kStaleEpoch;
    counters_.stale_epoch_rejections += 1;
    counters_.handshakes_rejected += 1;
  } else if (verdict == ClaimVerdict::kBootMismatch) {
    // A worker from a previous boot generation cannot inherit authority either.
    response.status = transport::HandshakeStatus::kStaleBoot;
    counters_.stale_boot_rejections += 1;
    counters_.handshakes_rejected += 1;
  } else if (verdict != ClaimVerdict::kAccepted) {
    response.status = transport::HandshakeStatus::kMalformed;
    counters_.handshakes_rejected += 1;
  } else {
    std::lock_guard<std::mutex> guard(mutex_);
    if (workers_.size() >= config_.max_workers && workers_.find(hello.worker) == workers_.end()) {
      response.status = transport::HandshakeStatus::kCapacityExceeded;
      counters_.capacity_rejections += 1;
      counters_.handshakes_rejected += 1;
    } else {
      WorkerRecord record;
      record.id = hello.worker;
      record.incarnation = hello.incarnation;
      record.boot = hello.boot;
      record.epoch = hello.epoch;
      record.first_tick = hello.first_tick;
      record.last_tick = hello.first_tick;
      record.connected = true;
      record.fenced = false;
      record.handshake = transport::HandshakeStatus::kAccepted;
      workers_[hello.worker] = record;
      response.status = transport::HandshakeStatus::kAccepted;
      counters_.handshakes_accepted += 1;
      registered = true;
      worker = hello.worker;
    }
  }

  transport::Frame ack;
  ack.header.type = transport::MessageType::kHelloAck;
  ack.header.seq = FrameSeq::from_raw(kFrameSeqStart);
  transport::encode(response, ByteWriter(ack.payload));
  if (!channel->send(ack).ok()) {
    finish(false);
    return;
  }
  if (response.status != transport::HandshakeStatus::kAccepted) {
    finish(false);
    return;
  }

  FrameSeq sequence = FrameSeq::from_raw(kFrameSeqStart);
  while (running_.load()) {
    received = channel->receive(frame);
    if (!received.ok()) {
      // The peer stopped talking: a closed socket, a crash or a hard kill all land here.
      finish(false);
      return;
    }
    counters_.frames_received += 1;
    sequence = FrameSeq::from_raw(sequence.raw() + 1U);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto found = workers_.find(worker);
      if (found != workers_.end()) {
        found->second.frames += 1;
      }
    }

    switch (frame.header.type) {
      case transport::MessageType::kHeartbeat: {
        transport::Heartbeat heartbeat;
        ByteReader heartbeat_reader(frame.payload);
        if (!transport::decode(heartbeat_reader, heartbeat).ok()) {
          counters_.frames_rejected += 1;
          static_cast<void>(send_error(channel, StatusCode::kCorrupt, "heartbeat payload invalid"));
          finish(false);
          return;
        }
        if (heartbeat.epoch != epoch_) {
          counters_.stale_epoch_rejections += 1;
          transport::Frame fence;
          fence.header.type = transport::MessageType::kFence;
          fence.header.seq = sequence;
          transport::FenceNotice notice;
          notice.epoch = epoch_;
          notice.coordinator = incarnation_;
          notice.attempt = AttemptId::from_raw(hash_combine(worker.raw(), sequence.raw()));
          transport::encode(notice, ByteWriter(fence.payload));
          static_cast<void>(channel->send(fence));
          finish(false);
          return;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        auto found = workers_.find(worker);
        if (found != workers_.end()) {
          found->second.last_tick = heartbeat.tick;
        }
        break;
      }
      case transport::MessageType::kEvidence: {
        transport::EvidenceBatch batch;
        ByteReader batch_reader(frame.payload);
        const Status decoded = transport::decode(batch_reader, batch);
        if (!decoded.ok()) {
          counters_.frames_rejected += 1;
          static_cast<void>(send_error(channel, decoded.code(), decoded.detail()));
          finish(false);
          return;
        }
        if (batch.epoch != epoch_) {
          counters_.stale_epoch_rejections += 1;
          transport::Frame fence;
          fence.header.type = transport::MessageType::kFence;
          fence.header.seq = sequence;
          transport::FenceNotice notice;
          notice.epoch = epoch_;
          notice.coordinator = incarnation_;
          notice.attempt = AttemptId::from_raw(hash_combine(worker.raw(), sequence.raw()));
          transport::encode(notice, ByteWriter(fence.payload));
          static_cast<void>(channel->send(fence));
          finish(false);
          return;
        }
        transport::EvidenceAck ack_message;
        ack_message.seq = frame.header.seq;
        ack_message.epoch = epoch_;
        for (const Sample& sample : batch.samples) {
          const IngestOutcome outcome = config_.governor->ingest(sample);
          if (outcome.status.ok()) {
            ack_message.accepted += 1;
            counters_.samples_ingested += 1;
            if (sample.tick.value() > 0) {
              std::lock_guard<std::mutex> guard(mutex_);
              auto found = workers_.find(worker);
              if (found != workers_.end()) {
                found->second.last_tick = sample.tick;
                found->second.samples += 1;
              }
            }
            ack_message.classification = static_cast<std::uint8_t>(outcome.classification);
          } else {
            ack_message.rejected += 1;
            counters_.samples_rejected += 1;
          }
        }
        transport::Frame response_frame;
        response_frame.header.type = transport::MessageType::kEvidenceAck;
        response_frame.header.seq = sequence;
        transport::encode(ack_message, ByteWriter(response_frame.payload));
        if (!channel->send(response_frame).ok()) {
          finish(false);
          return;
        }
        break;
      }
      case transport::MessageType::kGoodbye:
        finish(true);
        return;
      case transport::MessageType::kHello:
      case transport::MessageType::kHelloAck:
      case transport::MessageType::kEvidenceAck:
      case transport::MessageType::kFence:
      case transport::MessageType::kError:
        counters_.frames_rejected += 1;
        static_cast<void>(send_error(channel, StatusCode::kProtocol, "unexpected message type"));
        finish(false);
        return;
    }
  }
  finish(false);
}

std::vector<WorkerRecord> Coordinator::workers() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<WorkerRecord> out;
  out.reserve(workers_.size());
  for (const auto& [id, record] : workers_) {
    static_cast<void>(id);
    out.push_back(record);
  }
  std::sort(out.begin(), out.end(),
            [](const WorkerRecord& a, const WorkerRecord& b) { return a.id < b.id; });
  return out;
}

std::vector<WorkerRecord> Coordinator::fenced_workers() const {
  const std::vector<WorkerRecord> all = workers();
  std::vector<WorkerRecord> out;
  for (const WorkerRecord& record : all) {
    if (record.fenced) {
      out.push_back(record);
    }
  }
  return out;
}

CoordinatorStats Coordinator::stats() const {
  CoordinatorStats out;
  out.connections_accepted = counters_.connections_accepted.load();
  out.handshakes_accepted = counters_.handshakes_accepted.load();
  out.handshakes_rejected = counters_.handshakes_rejected.load();
  out.stale_epoch_rejections = counters_.stale_epoch_rejections.load();
  out.stale_boot_rejections = counters_.stale_boot_rejections.load();
  out.version_rejections = counters_.version_rejections.load();
  out.capacity_rejections = counters_.capacity_rejections.load();
  out.protocol_errors = counters_.protocol_errors.load();
  out.frames_received = counters_.frames_received.load();
  out.frames_rejected = counters_.frames_rejected.load();
  out.samples_ingested = counters_.samples_ingested.load();
  out.samples_rejected = counters_.samples_rejected.load();
  out.workers_fenced = counters_.workers_fenced.load();
  out.worker_disconnects = counters_.worker_disconnects.load();
  return out;
}

}  // namespace mbg
