// Microburst Governor - blocking socket transport.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "mbg/transport/socket.hpp"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mbg::transport {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr SOCKET kNativeInvalid = INVALID_SOCKET;
constexpr int kWouldBlock = WSAEWOULDBLOCK;
constexpr int kInterrupted = WSAEINTR;

native_socket to_native(Socket::handle_type handle) noexcept {
  return static_cast<native_socket>(handle);
}
Socket::handle_type from_native(native_socket handle) noexcept {
  return static_cast<Socket::handle_type>(handle);
}
#else
using native_socket = int;
constexpr int kNativeInvalid = -1;
constexpr int kWouldBlock = EWOULDBLOCK;
constexpr int kInterrupted = EINTR;

native_socket to_native(Socket::handle_type handle) noexcept {
  return static_cast<native_socket>(handle);
}
Socket::handle_type from_native(native_socket handle) noexcept {
  return static_cast<Socket::handle_type>(handle);
}
#endif

std::once_flag g_startup_once;
Status g_startup_status = Status{};

void close_native(native_socket handle) noexcept {
  if (handle == kNativeInvalid) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

Status last_error(const char* detail) {
#if defined(_WIN32)
  const int code = ::WSAGetLastError();
  if (code == kWouldBlock) {
    return Status::failure(StatusCode::kIo, detail);
  }
  return Status::failure(StatusCode::kIo, detail);
#else
  return Status::failure(StatusCode::kIo, detail);
#endif
}

}  // namespace

Status net_startup() noexcept {
  std::call_once(g_startup_once, []() {
#if defined(_WIN32)
    WSADATA data{};
    const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      g_startup_status = Status::failure(StatusCode::kIo, "Winsock initialisation failed");
    }
#else
    g_startup_status = Status{};
#endif
  });
  return g_startup_status;
}

void net_shutdown() noexcept {
#if defined(_WIN32)
  ::WSACleanup();
#endif
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

Socket::~Socket() { close(); }

Socket::handle_type Socket::release() noexcept {
  const handle_type handle = handle_;
  handle_ = kInvalid;
  return handle;
}

void Socket::close() noexcept {
  if (handle_ != kInvalid) {
    close_native(to_native(handle_));
    handle_ = kInvalid;
  }
}

void Socket::interrupt() noexcept {
  if (handle_ == kInvalid) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(to_native(handle_), SD_BOTH);
#else
  ::shutdown(to_native(handle_), SHUT_RDWR);
#endif
}

Status Socket::set_nodelay(bool enable) noexcept {
  if (!valid()) {
    return Status::failure(StatusCode::kIo, "socket is not open");
  }
  const int value = enable ? 1 : 0;
  if (::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::failure(StatusCode::kIo, "TCP_NODELAY could not be set");
  }
  return Status{};
}

Status Socket::send_all(std::span<const std::byte> bytes) noexcept {
  if (!valid()) {
    return Status::failure(StatusCode::kIo, "socket is not open");
  }
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t chunk = bytes.size() - sent;
    const int request = static_cast<int>(chunk > 1U << 20 ? 1U << 20 : chunk);
    const int result = ::send(to_native(handle_), reinterpret_cast<const char*>(bytes.data() + sent),
                              request, 0);
    if (result <= 0) {
      return last_error("socket send failed");
    }
    sent += static_cast<std::size_t>(result);
  }
  return Status{};
}

Status Socket::recv_all(std::span<std::byte> bytes) noexcept {
  if (!valid()) {
    return Status::failure(StatusCode::kIo, "socket is not open");
  }
  std::size_t received = 0;
  while (received < bytes.size()) {
    const std::size_t chunk = bytes.size() - received;
    const int request = static_cast<int>(chunk > 1U << 20 ? 1U << 20 : chunk);
    const int result = ::recv(to_native(handle_), reinterpret_cast<char*>(bytes.data() + received),
                              request, 0);
    if (result == 0) {
      return Status::failure(StatusCode::kIo, "peer closed the connection");
    }
    if (result < 0) {
      return last_error("socket receive failed");
    }
    received += static_cast<std::size_t>(result);
  }
  return Status{};
}

Listener::~Listener() { close(); }

Status Listener::bind_loopback(std::uint16_t port, std::size_t backlog) {
  const Status started = net_startup();
  if (!started.ok()) {
    return started;
  }
  native_socket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kNativeInvalid) {
    return Status::failure(StatusCode::kIo, "listener socket could not be created");
  }
  const int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  address.sin_port = ::htons(port);
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(handle);
    return Status::failure(StatusCode::kIo, "listener could not bind the loopback port");
  }
  if (::listen(handle, static_cast<int>(backlog)) != 0) {
    close_native(handle);
    return Status::failure(StatusCode::kIo, "listener could not listen");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(bound));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    close_native(handle);
    return Status::failure(StatusCode::kIo, "listener port could not be read back");
  }
  socket_ = Socket(from_native(handle));
  port_ = ::ntohs(bound.sin_port);
  return Status{};
}

Status Listener::accept(Socket& out) noexcept {
  if (!socket_.valid()) {
    return Status::failure(StatusCode::kCancelled, "listener is closed");
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(peer));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(peer));
#endif
  const native_socket handle =
      ::accept(to_native(socket_.handle()), reinterpret_cast<sockaddr*>(&peer), &length);
  if (handle == kNativeInvalid) {
    return Status::failure(StatusCode::kCancelled, "listener accept was interrupted");
  }
  out = Socket(from_native(handle));
  return Status{};
}

void Listener::interrupt() noexcept { socket_.interrupt(); }

void Listener::close() noexcept {
  if (socket_.valid()) {
    socket_.interrupt();
    socket_.close();
  }
}

Status connect_loopback(const char* host, std::uint16_t port, Socket& out) noexcept {
  const Status started = net_startup();
  if (!started.ok()) {
    return started;
  }
  native_socket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kNativeInvalid) {
    return Status::failure(StatusCode::kIo, "client socket could not be created");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, host, &address.sin_addr) != 1) {
    close_native(handle);
    return Status::failure(StatusCode::kInvalidArgument, "connect host is not a valid IPv4 address");
  }
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(handle);
    return Status::failure(StatusCode::kIo, "connection to the coordinator was refused");
  }
  out = Socket(from_native(handle));
  return Status{};
}

Status FramedChannel::send(const Frame& frame) noexcept {
  std::vector<std::byte> image;
  const Status encoded = encode_frame(frame, image);
  if (!encoded.ok()) {
    return encoded;
  }
  const Status written = socket_.send_all(image);
  if (!written.ok()) {
    return written;
  }
  frames_sent_ += 1;
  return Status{};
}

Status FramedChannel::receive(Frame& frame) noexcept {
  std::vector<std::byte> header_bytes(kFrameHeaderBytes);
  const Status header_status = socket_.recv_all(header_bytes);
  if (!header_status.ok()) {
    return header_status;
  }
  FrameHeader header;
  const Status decoded = decode_header(header_bytes, header);
  if (!decoded.ok()) {
    return decoded;
  }
  std::vector<std::byte> image(kFrameHeaderBytes + header.payload_length);
  std::memcpy(image.data(), header_bytes.data(), kFrameHeaderBytes);
  if (header.payload_length != 0) {
    const Status body_status =
        socket_.recv_all(std::span<std::byte>(image.data() + kFrameHeaderBytes, header.payload_length));
    if (!body_status.ok()) {
      return body_status;
    }
  }
  const Status verified = decode_frame(image, frame);
  if (!verified.ok()) {
    return verified;
  }
  frames_received_ += 1;
  return Status{};
}

}  // namespace mbg::transport
