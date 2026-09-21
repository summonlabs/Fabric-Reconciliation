// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Loopback reconciliation service.
//
// The service exposes the engine over the bounded framed protocol. Each
// accepted connection establishes a session carrying the client boot identity,
// the coordinator epoch and the store identity observed at handshake time.
// Every request is checked against that established session: a frame whose
// session id, client boot identity or epoch does not match is refused, so one
// session can never act under another session's identity, boot or epoch.
//
// Shutdown is cooperative and deterministic: the listening socket and every
// established connection are shut down, which releases blocked accepts and
// blocked reads, and only then are the service threads joined.

#ifndef SUMMON_FABRIC_RECONCILIATION_SERVER_HPP
#define SUMMON_FABRIC_RECONCILIATION_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/engine.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/wire.hpp"

namespace summon {
namespace fabric_reconciliation {

struct FR_API ServerOptions {
  std::string bind_address{"127.0.0.1"};
  /// Zero asks the operating system for an ephemeral port.
  std::uint16_t port{0};
  RuntimeLimits limits{};
  std::size_t max_sessions{64};
  /// When true the service accepts exactly one session and then stops
  /// accepting. Used by deterministic lifecycle proofs.
  bool single_session{false};
};

struct FR_API SessionSnapshot {
  std::uint64_t session_id{0};
  BootId client_boot;
  CoordinatorEpoch handshake_epoch;
  std::uint64_t requests_served{0};
  std::uint64_t rejected_frames{0};

  friend bool operator==(const SessionSnapshot&, const SessionSnapshot&) = default;
};

class FR_API ReconciliationServer {
 public:
  ~ReconciliationServer();
  ReconciliationServer(const ReconciliationServer&) = delete;
  ReconciliationServer& operator=(const ReconciliationServer&) = delete;

  [[nodiscard]] static Status Start(const ServerOptions& options, ReconciliationEngine& engine,
                                    std::unique_ptr<ReconciliationServer>& out);

  /// Stops accepting, shuts every connection down and joins every thread.
  /// Idempotent. Returns only after all threads have finished.
  [[nodiscard]] Status Stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::string endpoint() const;
  [[nodiscard]] std::vector<SessionSnapshot> sessions() const;
  [[nodiscard]] std::uint64_t connections_accepted() const noexcept;
  [[nodiscard]] std::uint64_t frames_rejected() const noexcept;

 private:
  ReconciliationServer() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_{0};
};

/// A blocking client over the framed protocol.
class FR_API ReconciliationClient {
 public:
  ReconciliationClient() = default;
  ReconciliationClient(ReconciliationClient&&) noexcept;
  ReconciliationClient& operator=(ReconciliationClient&&) noexcept;
  ReconciliationClient(const ReconciliationClient&) = delete;
  ReconciliationClient& operator=(const ReconciliationClient&) = delete;
  ~ReconciliationClient();

  [[nodiscard]] static Status Connect(const std::string& host, std::uint16_t port,
                                      const RuntimeLimits& limits,
                                      ReconciliationClient& out);

  /// Performs one request/response exchange. When the service reports that the
  /// caller's epoch is no longer current, the client re-handshakes once and
  /// retries, so a restart of the service surfaces as a new epoch rather than
  /// as a silent success.
  [[nodiscard]] Status Call(WireOperation operation, std::string_view payload,
                            std::string& response);

  [[nodiscard]] bool connected() const noexcept { return socket_ != nullptr; }
  [[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }
  [[nodiscard]] CoordinatorEpoch server_epoch() const noexcept { return server_epoch_; }
  [[nodiscard]] BootId server_boot() const noexcept { return server_boot_; }
  [[nodiscard]] StoreId server_store() const noexcept { return server_store_; }
  [[nodiscard]] std::uint64_t rehandshakes() const noexcept { return rehandshakes_; }

  void Close();

 private:
  [[nodiscard]] Status Handshake();
  [[nodiscard]] Status Exchange(WireMessageType type, std::string_view payload,
                                FrameHeader& header, std::string& response);
  /// Receives one frame, releasing deterministically when Close() is called from
  /// another thread.
  [[nodiscard]] bool ReceiveOne(FrameReader& reader, FrameHeader& header, std::string& payload);

  /// Owned through a shared pointer so that a call already in flight keeps the
  /// descriptor alive while Close() releases it.
  std::shared_ptr<void> socket_;
  /// Releases a blocked read in Exchange. Opaque here to keep the transport
  /// layer out of the public header.
  std::shared_ptr<void> wake_;
  std::uint64_t session_id_{0};
  std::uint64_t sequence_{0};
  CoordinatorEpoch server_epoch_;
  BootId server_boot_;
  StoreId server_store_;
  BootId client_boot_;
  std::uint64_t rehandshakes_{0};
  RuntimeLimits limits_{};
  std::string host_;
  std::uint16_t port_{0};
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_SERVER_HPP
