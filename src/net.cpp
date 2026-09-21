// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "net.hpp"

#include <cerrno>
#include <cstring>

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

namespace summon {
namespace fabric_reconciliation {
namespace net {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

void EnsureWsa() {
  static bool started = false;
  if (!started) {
    WSADATA data;
    (void)::WSAStartup(MAKEWORD(2, 2), &data);
    started = true;
  }
}

void ReleaseWsa() {
  static bool released = false;
  if (!released) {
    ::WSACleanup();
    released = true;
  }
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

NativeSocket ToNative(void* handle) noexcept {
  return static_cast<NativeSocket>(reinterpret_cast<std::intptr_t>(handle));
}

void* FromNative(NativeSocket socket) noexcept {
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(socket));
}

}  // namespace

void EnsureStarted() noexcept {
#if defined(_WIN32)
  EnsureWsa();
#endif
}

void EnsureStopped() noexcept {
#if defined(_WIN32)
  ReleaseWsa();
#endif
}

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_.exchange(nullptr, std::memory_order_acq_rel)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    Close();
    handle_.store(other.handle_.exchange(nullptr, std::memory_order_acq_rel),
                  std::memory_order_release);
  }
  return *this;
}

Socket::~Socket() { Close(); }

void Socket::Close() noexcept {
  // Exactly one thread wins the exchange, so a close can never be applied twice
  // to the same descriptor even when shutdown races with the owning thread.
  void* raw = handle_.exchange(nullptr, std::memory_order_acq_rel);
  if (raw == nullptr) {
    return;
  }
  const NativeSocket socket = ToNative(raw);
#if defined(_WIN32)
  ::shutdown(socket, SD_BOTH);
  ::closesocket(socket);
#else
  ::shutdown(socket, SHUT_RDWR);
  ::close(socket);
#endif
}

void Socket::Shutdown() noexcept {
  void* raw = handle_.load(std::memory_order_acquire);
  if (raw == nullptr) {
    return;
  }
  const NativeSocket socket = ToNative(raw);
#if defined(_WIN32)
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

Status Listen(const std::string& address, std::uint16_t port, Socket& out,
              std::uint16_t& effective_port) {
  EnsureStarted();
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot create listening socket");
  }
  int reuse = 1;
  (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "bind address is not a valid IPv4 literal");
  }
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached, "bind failed");
  }
  if (::listen(socket, 8) != 0) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached, "listen failed");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = static_cast<int>(sizeof(bound));
#else
  socklen_t bound_length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached, "getsockname failed");
  }
  effective_port = ntohs(bound.sin_port);
  out = Socket(FromNative(socket));
  return Status::Ok();
}

Status Accept(const Socket& listener, Socket& out) {
  if (!listener.valid()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed,
                  "listener is not open");
  }
  const NativeSocket socket = ToNative(listener.handle());
#if defined(_WIN32)
  const SOCKET accepted = ::accept(socket, nullptr, nullptr);
  if (accepted == INVALID_SOCKET) {
    const int error = ::WSAGetLastError();
    if (error == WSAEINTR || error == WSAENOTSOCK || error == WSAEINVAL ||
        error == WSAENETDOWN || error == WSAEOPNOTSUPP) {
      return Status(StatusCode::NotFound, ReasonCode::SessionClosed,
                    "listener was shut down while waiting for a connection");
    }
    return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed, "accept failed");
  }
  out = Socket(FromNative(accepted));
#else
  const int accepted = ::accept(socket, nullptr, nullptr);
  if (accepted < 0) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed,
                  "listener was shut down while waiting for a connection");
  }
  out = Socket(FromNative(accepted));
#endif
  return Status::Ok();
}

Status WakeupChannel::Create(WakeupChannel& out) {
  EnsureStarted();
  const NativeSocket receiver = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (receiver == kInvalidSocket) {
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot create wakeup receiver");
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = 0;
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(receiver, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    Socket(reinterpret_cast<void*>(static_cast<std::intptr_t>(receiver))).Close();
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot bind wakeup receiver");
  }
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(endpoint));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(endpoint));
#endif
  if (::getsockname(receiver, reinterpret_cast<sockaddr*>(&endpoint), &length) != 0) {
    Socket(reinterpret_cast<void*>(static_cast<std::intptr_t>(receiver))).Close();
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot inspect wakeup receiver");
  }
  const NativeSocket sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sender == kInvalidSocket) {
    Socket(reinterpret_cast<void*>(static_cast<std::intptr_t>(receiver))).Close();
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot create wakeup sender");
  }
  if (::connect(sender, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
    Socket(reinterpret_cast<void*>(static_cast<std::intptr_t>(receiver))).Close();
    Socket(reinterpret_cast<void*>(static_cast<std::intptr_t>(sender))).Close();
    return Status(StatusCode::Rejected, ReasonCode::SessionLimitReached,
                  "cannot connect wakeup sender");
  }
  WakeupChannel channel;
  channel.receiver_ = Socket(FromNative(receiver));
  channel.sender_ = Socket(FromNative(sender));
  out = std::move(channel);
  return Status::Ok();
}

void WakeupChannel::Signal() noexcept {
  if (!sender_.valid()) {
    return;
  }
  const char byte = 1;
  (void)::send(ToNative(sender_.handle()), &byte, 1, 0);
}

void WakeupChannel::Close() noexcept {
  sender_.Close();
  receiver_.Close();
}

Status WaitReadable(const Socket& listener, const WakeupChannel& wake, bool& listener_ready) {
  listener_ready = false;
  if (!listener.valid()) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "listener is not open");
  }
  const NativeSocket listener_native = ToNative(listener.handle());
  NativeSocket wake_native = kInvalidSocket;
  if (wake.valid()) {
    wake_native = ToNative(wake.receiver().handle());
  }

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(listener_native, &read_set);
  NativeSocket highest = listener_native;
  if (wake_native != kInvalidSocket) {
    FD_SET(wake_native, &read_set);
    if (wake_native > highest) {
      highest = wake_native;
    }
  }

#if defined(_WIN32)
  const int ready = ::select(0, &read_set, nullptr, nullptr, nullptr);
#else
  const int ready = ::select(static_cast<int>(highest) + 1, &read_set, nullptr, nullptr, nullptr);
#endif
  if (ready < 0) {
#if defined(_WIN32)
    const int error = ::WSAGetLastError();
    if (error == WSAEINTR || error == WSAENOTSOCK || error == WSAEINVAL) {
      return Status(StatusCode::NotFound, ReasonCode::SessionClosed,
                    "wait was released while the listener was closing");
    }
#else
    if (errno == EINTR) {
      return Status(StatusCode::NotFound, ReasonCode::SessionClosed,
                    "wait was interrupted while the listener was closing");
    }
#endif
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "select failed");
  }
  if (wake_native != kInvalidSocket && FD_ISSET(wake_native, &read_set)) {
    char drained[64];
    (void)::recv(wake_native, drained, sizeof(drained), 0);
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "service is shutting down");
  }
  if (!FD_ISSET(listener_native, &read_set)) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "no readable socket");
  }
  listener_ready = true;
  return Status::Ok();
}

Status Connect(const std::string& host, std::uint16_t port, Socket& out) {
  EnsureStarted();
  const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(StatusCode::Rejected, ReasonCode::SessionClosed, "cannot create socket");
  }
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &endpoint.sin_addr) != 1) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::Rejected, ReasonCode::SessionClosed,
                  "host is not a valid IPv4 literal");
  }
  int nodelay = 1;
  (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "connect failed");
  }
  out = Socket(FromNative(socket));
  return Status::Ok();
}

Status SendAll(const Socket& socket, const void* data, std::size_t size) {
  if (!socket.valid()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed, "socket is closed");
  }
  const NativeSocket native = ToNative(socket.handle());
  const auto* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < size) {
#if defined(_WIN32)
    const int chunk = static_cast<int>(size - sent);
    const int written = ::send(native, bytes + sent, chunk, 0);
#else
    const ssize_t written = ::send(native, bytes + sent, size - sent, MSG_NOSIGNAL);
#endif
    if (written <= 0) {
      return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed, "send failed");
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::Ok();
}

Status Receive(const Socket& socket, void* data, std::size_t capacity, std::size_t& received) {
  received = 0;
  if (!socket.valid()) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "socket is closed");
  }
  const NativeSocket native = ToNative(socket.handle());
#if defined(_WIN32)
  const int chunk = static_cast<int>(capacity);
  const int read = ::recv(native, static_cast<char*>(data), chunk, 0);
  if (read == 0) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "peer closed the connection");
  }
  if (read < 0) {
    return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed, "receive failed");
  }
  received = static_cast<std::size_t>(read);
#else
  const ssize_t read = ::recv(native, data, capacity, 0);
  if (read == 0) {
    return Status(StatusCode::NotFound, ReasonCode::SessionClosed, "peer closed the connection");
  }
  if (read < 0) {
    return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed, "receive failed");
  }
  received = static_cast<std::size_t>(read);
#endif
  return Status::Ok();
}

std::string DescribePeer(const Socket& socket) {
  if (!socket.valid()) {
    return "<closed>";
  }
  const NativeSocket native = ToNative(socket.handle());
  sockaddr_in peer{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(peer));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(peer));
#endif
  if (::getpeername(native, reinterpret_cast<sockaddr*>(&peer), &length) != 0) {
    return "<unknown>";
  }
  char buffer[INET_ADDRSTRLEN] = {};
  if (::inet_ntop(AF_INET, &peer.sin_addr, buffer, sizeof(buffer)) == nullptr) {
    return "<unknown>";
  }
  std::string result(buffer);
  result += ':';
  result += std::to_string(ntohs(peer.sin_port));
  return result;
}

std::string LastErrorMessage() {
#if defined(_WIN32)
  const int error = ::WSAGetLastError();
  return "winsock error " + std::to_string(error);
#else
  return std::string(std::strerror(errno));
#endif
}

}  // namespace net
}  // namespace fabric_reconciliation
}  // namespace summon
