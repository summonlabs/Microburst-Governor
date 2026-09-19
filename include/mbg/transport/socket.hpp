// Microburst Governor - blocking socket transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "mbg/core/status.hpp"
#include "mbg/transport/frame.hpp"

namespace mbg::transport {

/// One-time process wide network initialisation. Safe to call repeatedly.
[[nodiscard]] Status net_startup() noexcept;
void net_shutdown() noexcept;

/// An owned socket handle. Blocking mode; there are no socket timeouts anywhere in this runtime, so
/// a stalled peer is broken by shutdown() from the owning thread or by the peer closing the
/// connection, never by a watchdog.
class Socket {
 public:
  using handle_type = std::uintptr_t;
  static constexpr handle_type kInvalid = ~handle_type{0};

  Socket() = default;
  explicit Socket(handle_type handle) noexcept : handle_(handle) {}
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket();

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalid; }
  [[nodiscard]] handle_type handle() const noexcept { return handle_; }
  handle_type release() noexcept;
  void close() noexcept;

  [[nodiscard]] Status set_nodelay(bool enable) noexcept;
  [[nodiscard]] Status send_all(std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] Status recv_all(std::span<std::byte> bytes) noexcept;

  /// Breaks a blocking send or receive on this socket from another thread. Idempotent.
  void interrupt() noexcept;

 private:
  handle_type handle_{kInvalid};
};

/// A loopback listener.
class Listener {
 public:
  Listener() = default;
  Listener(Listener&&) noexcept = default;
  Listener& operator=(Listener&&) noexcept = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  ~Listener();

  /// Binds on loopback. Port 0 selects an ephemeral port; the chosen port is reported by port().
  [[nodiscard]] Status bind_loopback(std::uint16_t port, std::size_t backlog = 64);
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

  /// Blocks until a connection arrives, the listener is interrupted, or the peer aborts.
  [[nodiscard]] Status accept(Socket& out) noexcept;

  void interrupt() noexcept;
  void close() noexcept;

 private:
  Socket socket_{};
  std::uint16_t port_{0};
};

/// A length framed channel over a socket. Enforces the frame bound on both directions.
class FramedChannel {
 public:
  explicit FramedChannel(Socket socket) noexcept : socket_(std::move(socket)) {}
  FramedChannel(FramedChannel&&) noexcept = default;
  FramedChannel& operator=(FramedChannel&&) noexcept = default;
  FramedChannel(const FramedChannel&) = delete;
  FramedChannel& operator=(const FramedChannel&) = delete;

  [[nodiscard]] Status send(const Frame& frame) noexcept;
  [[nodiscard]] Status receive(Frame& frame) noexcept;
  void interrupt() noexcept { socket_.interrupt(); }
  void close() noexcept { socket_.close(); }
  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_; }
  [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }
  [[nodiscard]] Socket& socket() noexcept { return socket_; }

 private:
  Socket socket_{};
  std::uint64_t frames_sent_{0};
  std::uint64_t frames_received_{0};
};

/// Connects to a loopback port.
[[nodiscard]] Status connect_loopback(const char* host, std::uint16_t port, Socket& out) noexcept;

}  // namespace mbg::transport
