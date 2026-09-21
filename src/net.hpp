// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal blocking socket wrapper.
//
// Not installed: the transport is an implementation detail behind
// server.hpp. Shutdown is cooperative and deterministic, with no timeouts and
// no polling: the listener waits on an event that shutdown sets, and a blocked
// receive is released by shutting the socket down.

#ifndef SUMMON_FABRIC_RECONCILIATION_NET_HPP
#define SUMMON_FABRIC_RECONCILIATION_NET_HPP

#include <atomic>
#include <cstdint>
#include <string>

#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace net {

/// One-time process-wide socket subsystem initialisation.
void EnsureStarted() noexcept;
void EnsureStopped() noexcept;

class Socket {
 public:
  Socket() = default;
  explicit Socket(void* handle) : handle_(handle) {}
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket();

  [[nodiscard]] bool valid() const noexcept {
    return handle_.load(std::memory_order_acquire) != nullptr;
  }
  [[nodiscard]] void* handle() const noexcept {
    return handle_.load(std::memory_order_acquire);
  }

  /// Closes the socket. Idempotent. Safe to call while another thread is
  /// blocked in Receive or Accept.
  void Close() noexcept;

  /// Releases a blocked Receive or Accept without closing the descriptor.
  void Shutdown() noexcept;

  void Release() noexcept { handle_.store(nullptr, std::memory_order_release); }

 private:
  std::atomic<void*> handle_{nullptr};
};

/// Creates a loopback-capable listening socket. A port of zero asks the
/// operating system for an ephemeral port; the effective port is returned.
[[nodiscard]] Status Listen(const std::string& address, std::uint16_t port, Socket& out,
                            std::uint16_t& effective_port);

/// A loopback datagram pair used to release a blocked wait deterministically.
///
/// A blocking accept cannot be relied upon to return when a listening socket is
/// shut down on every supported platform, and the runtime never uses timeouts
/// to paper over that. The accept loop therefore waits on the listener and on
/// this channel together, and shutdown signals the channel.
class WakeupChannel {
 public:
  WakeupChannel() = default;
  WakeupChannel(WakeupChannel&&) noexcept = default;
  WakeupChannel& operator=(WakeupChannel&&) noexcept = default;
  WakeupChannel(const WakeupChannel&) = delete;
  WakeupChannel& operator=(const WakeupChannel&) = delete;

  [[nodiscard]] static Status Create(WakeupChannel& out);

  /// Releases a thread blocked in WaitReadable. Safe to call repeatedly and
  /// from any thread.
  void Signal() noexcept;
  void Close() noexcept;

  [[nodiscard]] const Socket& receiver() const noexcept { return receiver_; }
  [[nodiscard]] bool valid() const noexcept { return receiver_.valid(); }

 private:
  Socket receiver_;
  Socket sender_;
};

/// Blocks until @p listener has a pending connection or @p wake is signalled.
/// Sets @p listener_ready accordingly. Returns StatusCode::NotFound when the
/// wait ended because the listener itself became unusable.
[[nodiscard]] Status WaitReadable(const Socket& listener, const WakeupChannel& wake,
                                  bool& listener_ready);

/// Blocks until a connection arrives or the listener is shut down. Returns
/// StatusCode::NotFound when the listener was shut down instead.
[[nodiscard]] Status Accept(const Socket& listener, Socket& out);

/// Connects to a loopback endpoint. Uses a blocking connect: the runtime is
/// not permitted to hide lifecycle failures behind timeouts.
[[nodiscard]] Status Connect(const std::string& host, std::uint16_t port, Socket& out);

/// Sends every byte or fails.
[[nodiscard]] Status SendAll(const Socket& socket, const void* data, std::size_t size);

/// Receives at least one byte. Returns StatusCode::NotFound when the peer
/// closed the connection cleanly, and StatusCode::Indeterminate when the
/// socket was shut down locally.
[[nodiscard]] Status Receive(const Socket& socket, void* data, std::size_t capacity,
                             std::size_t& received);

/// Human-readable peer description, used for session diagnostics.
[[nodiscard]] std::string DescribePeer(const Socket& socket);

/// Describes the last socket error of the calling thread.
[[nodiscard]] std::string LastErrorMessage();

}  // namespace net
}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_NET_HPP
