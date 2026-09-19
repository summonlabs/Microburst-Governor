// Microburst Governor - worker side of the framed authority transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "mbg/core/status.hpp"
#include "mbg/model/sample.hpp"
#include "mbg/transport/socket.hpp"

namespace mbg {

/// A worker process that submits evidence to a coordinator.
///
/// The worker holds no authority of its own. It presents its incarnation, boot and the epoch it
/// believes it holds, and it is told explicitly when that belief is stale.
class Worker {
 public:
  struct Config {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    WorkerId id{};
    IncarnationId incarnation{};
    BootId boot{};
    Epoch epoch{};
    Tick first_tick{};
  };

  Worker() = default;
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  ~Worker();

  [[nodiscard]] Status connect(const Config& config);
  [[nodiscard]] Status handshake(transport::HelloResponse& response);
  [[nodiscard]] Status send_heartbeat(Tick tick);
  [[nodiscard]] Status send_evidence(const transport::EvidenceBatch& batch,
                                     transport::EvidenceAck* ack);
  [[nodiscard]] Status say_goodbye();
  void close();

  [[nodiscard]] bool connected() const noexcept { return channel_ != nullptr && channel_->valid(); }
  [[nodiscard]] Epoch granted_epoch() const noexcept { return granted_epoch_; }
  [[nodiscard]] transport::HandshakeStatus handshake_status() const noexcept {
    return handshake_status_;
  }
  [[nodiscard]] bool fenced() const noexcept { return fenced_; }
  [[nodiscard]] std::uint64_t frames_sent() const noexcept {
    return channel_ == nullptr ? 0 : channel_->frames_sent();
  }

 private:
  Config config_{};
  std::unique_ptr<transport::FramedChannel> channel_{};
  FrameSeq sequence_{FrameSeq::from_raw(1)};
  Epoch granted_epoch_{};
  transport::HandshakeStatus handshake_status_{transport::HandshakeStatus::kMalformed};
  bool fenced_{false};
};

}  // namespace mbg
