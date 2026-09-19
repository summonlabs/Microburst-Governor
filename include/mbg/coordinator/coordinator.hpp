// Microburst Governor - coordinator authority.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mbg/core/status.hpp"
#include "mbg/governor.hpp"
#include "mbg/persist/journal.hpp"
#include "mbg/transport/socket.hpp"

namespace mbg {

/// What the coordinator knows about one worker incarnation.
struct WorkerRecord {
  WorkerId id{};
  IncarnationId incarnation{};
  BootId boot{};
  Epoch epoch{};
  Tick first_tick{};
  Tick last_tick{};
  std::uint64_t frames{0};
  std::uint64_t samples{0};
  bool connected{false};
  bool fenced{false};
  transport::HandshakeStatus handshake{transport::HandshakeStatus::kAccepted};
  AttemptId fence_attempt{};
};

struct CoordinatorStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t handshakes_accepted{0};
  std::uint64_t handshakes_rejected{0};
  std::uint64_t stale_epoch_rejections{0};
  std::uint64_t stale_boot_rejections{0};
  std::uint64_t version_rejections{0};
  std::uint64_t capacity_rejections{0};
  std::uint64_t protocol_errors{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t samples_ingested{0};
  std::uint64_t samples_rejected{0};
  std::uint64_t workers_fenced{0};
  std::uint64_t worker_disconnects{0};
};

/// Owns the authority epoch for a deployment.
///
/// The coordinator is the only component that advances the epoch. It advances on every start, so a
/// restarted coordinator can never accept work that was authorised by the previous incarnation, and
/// a worker that presents an old epoch is rejected explicitly rather than silently admitted.
class Coordinator {
 public:
  struct Config {
    std::uint16_t port{0};
    Epoch seed_epoch{Epoch::from_raw(1)};
    BootId seed_boot{BootId::from_raw(1)};
    IncarnationId incarnation{};
    Governor* governor{nullptr};
    persist::DurableStore* store{nullptr};
    bool recover_authority{true};
    std::size_t max_workers{limits::kMaxConnections};
  };

  Coordinator();
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  [[nodiscard]] Status start(const Config& config);
  void stop();

  /// Blocks until every active session has ended. Returns false only when the coordinator was never
  /// started, so a shutdown path can never be mistaken for a completed one.
  [[nodiscard]] bool wait_for_sessions();

  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] BootId boot() const noexcept { return boot_; }
  [[nodiscard]] IncarnationId incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] bool authority_recovered() const noexcept { return authority_recovered_; }
  [[nodiscard]] Epoch previous_epoch() const noexcept { return previous_epoch_; }

  [[nodiscard]] std::vector<WorkerRecord> workers() const;
  [[nodiscard]] std::vector<WorkerRecord> fenced_workers() const;
  [[nodiscard]] CoordinatorStats stats() const;

  /// Reads the durable authority (epoch, boot) written by a previous incarnation.
  [[nodiscard]] static Status recover_durable_authority(persist::DurableStore& store, Epoch& epoch,
                                                        BootId& boot, bool& present);

 private:
  void run_accept_loop();
  void run_session(const std::shared_ptr<transport::FramedChannel>& channel);
  void register_session(const std::shared_ptr<transport::FramedChannel>& channel);
  void retire_session(const std::shared_ptr<transport::FramedChannel>& channel);
  [[nodiscard]] Status send_error(const std::shared_ptr<transport::FramedChannel>& channel,
                                  StatusCode code, const char* detail);

  Config config_{};
  transport::Listener listener_{};
  std::thread accept_thread_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::uint16_t port_{0};
  Epoch epoch_{Epoch::from_raw(1)};
  BootId boot_{BootId::from_raw(1)};
  IncarnationId incarnation_{};
  bool authority_recovered_{false};
  Epoch previous_epoch_{};

  /// Lock free counters.
  ///
  /// The session path must be able to account for a frame without taking the registry mutex, because
  /// the shutdown path waits on that mutex: a counter behind the mutex would couple the two and make
  /// shutdown wait for work it is trying to stop.
  struct Counters {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> handshakes_accepted{0};
    std::atomic<std::uint64_t> handshakes_rejected{0};
    std::atomic<std::uint64_t> stale_epoch_rejections{0};
    std::atomic<std::uint64_t> stale_boot_rejections{0};
    std::atomic<std::uint64_t> version_rejections{0};
    std::atomic<std::uint64_t> capacity_rejections{0};
    std::atomic<std::uint64_t> protocol_errors{0};
    std::atomic<std::uint64_t> frames_received{0};
    std::atomic<std::uint64_t> frames_rejected{0};
    std::atomic<std::uint64_t> samples_ingested{0};
    std::atomic<std::uint64_t> samples_rejected{0};
    std::atomic<std::uint64_t> workers_fenced{0};
    std::atomic<std::uint64_t> worker_disconnects{0};
  };

  mutable std::mutex mutex_{};
  std::condition_variable sessions_cv_{};
  std::size_t active_sessions_{0};
  std::vector<std::shared_ptr<transport::FramedChannel>> session_channels_{};
  std::unordered_map<WorkerId, WorkerRecord> workers_{};
  Counters counters_{};
};

}  // namespace mbg
