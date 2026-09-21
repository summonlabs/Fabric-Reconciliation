// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/server.hpp"

#include <array>
#include <cstdio>

#include "net.hpp"
#include "wire_ops.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

FrameHeader MakeHeader(WireMessageType type, std::uint64_t session_id, std::uint64_t sequence) {
  FrameHeader header;
  header.protocol_version = kWireProtocolVersion;
  header.type = type;
  header.flags = 0;
  header.session_id = session_id;
  header.sequence = sequence;
  return header;
}

std::string MakeNonce() {
  const BootId entropy = BootId::Generate();
  return std::string(reinterpret_cast<const char*>(entropy.bytes().data()),
                     entropy.bytes().size());
}

}  // namespace

ReconciliationClient::ReconciliationClient(ReconciliationClient&& other) noexcept
    : socket_(std::move(other.socket_)),
      wake_(std::move(other.wake_)),
      session_id_(other.session_id_),
      sequence_(other.sequence_),
      server_epoch_(other.server_epoch_),
      server_boot_(other.server_boot_),
      server_store_(other.server_store_),
      client_boot_(other.client_boot_),
      rehandshakes_(other.rehandshakes_),
      limits_(other.limits_),
      host_(std::move(other.host_)),
      port_(other.port_) {
  other.socket_.reset();
  other.wake_.reset();
}

ReconciliationClient& ReconciliationClient::operator=(ReconciliationClient&& other) noexcept {
  if (this != &other) {
    Close();
    socket_ = std::move(other.socket_);
    wake_ = std::move(other.wake_);
    session_id_ = other.session_id_;
    sequence_ = other.sequence_;
    server_epoch_ = other.server_epoch_;
    server_boot_ = other.server_boot_;
    server_store_ = other.server_store_;
    client_boot_ = other.client_boot_;
    rehandshakes_ = other.rehandshakes_;
    limits_ = other.limits_;
    host_ = std::move(other.host_);
    port_ = other.port_;
    other.socket_.reset();
    other.wake_.reset();
  }
  return *this;
}

ReconciliationClient::~ReconciliationClient() { Close(); }

void ReconciliationClient::Close() {
  // Releasing a blocked read is done through the wake channel rather than by
  // closing the descriptor underneath the reader: a blocking recv is not
  // guaranteed to return when a socket is shut down on every supported
  // platform. A call already in flight keeps its own reference to the socket,
  // so the descriptor stays valid until that call returns.
  if (wake_ != nullptr) {
    std::static_pointer_cast<net::WakeupChannel>(wake_)->Signal();
  }
  if (socket_ != nullptr) {
    std::static_pointer_cast<net::Socket>(socket_)->Shutdown();
  }
  socket_.reset();
  wake_.reset();
  session_id_ = 0;
  sequence_ = 0;
}

bool ReconciliationClient::ReceiveOne(FrameReader& reader, FrameHeader& header,
                                     std::string& payload) {
  auto socket = std::static_pointer_cast<net::Socket>(socket_);
  auto wake = std::static_pointer_cast<net::WakeupChannel>(wake_);
  if (socket == nullptr || wake == nullptr) {
    return false;
  }
  for (;;) {
    if (reader.failed()) {
      return false;
    }
    if (reader.Next(header, payload)) {
      return true;
    }
    if (reader.failed()) {
      return false;
    }
    bool ready = false;
    const Status waited = net::WaitReadable(*socket, *wake, ready);
    if (!waited.ok()) {
      return false;
    }
    if (!ready) {
      continue;
    }
    char buffer[4096];
    std::size_t received = 0;
    const Status status = net::Receive(*socket, buffer, sizeof(buffer), received);
    if (!status.ok() || received == 0) {
      return false;
    }
    reader.Feed(std::string_view(buffer, received));
  }
}

Status ReconciliationClient::Connect(const std::string& host, std::uint16_t port,
                                     const RuntimeLimits& limits, ReconciliationClient& out) {
  ReconciliationClient client;
  client.limits_ = limits;
  client.host_ = host;
  client.port_ = port;
  client.client_boot_ = BootId::Generate();
  auto socket = std::make_shared<net::Socket>();
  const Status connected = net::Connect(host, port, *socket);
  if (!connected.ok()) {
    return connected;
  }
  auto wake = std::make_shared<net::WakeupChannel>();
  const Status wake_created = net::WakeupChannel::Create(*wake);
  if (!wake_created.ok()) {
    return wake_created;
  }
  client.socket_ = socket;
  client.wake_ = wake;
  const Status handshake = client.Handshake();
  if (!handshake.ok()) {
    client.Close();
    return handshake;
  }
  out = std::move(client);
  return Status::Ok();
}

Status ReconciliationClient::Handshake() {
  if (socket_ == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed,
                  "client is not connected");
  }
  auto socket = std::static_pointer_cast<net::Socket>(socket_);
  const std::string hello = wireops::EncodeHello(client_boot_, MakeNonce());
  if (hello.empty()) {
    return Status(StatusCode::InternalError, ReasonCode::SessionClosed,
                  "hello payload could not be encoded");
  }
  const std::string image = EncodeFrame(MakeHeader(WireMessageType::Hello, 0, 1), hello);
  const Status sent = net::SendAll(*socket, image.data(), image.size());
  if (!sent.ok()) {
    return sent;
  }
  FrameReader reader(static_cast<std::uint32_t>(limits_.max_wire_payload_bytes));
  FrameHeader header;
  std::string payload;
  if (!ReceiveOne(reader, header, payload)) {
    return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed,
                  "service did not answer the handshake");
  }
  if (header.type != WireMessageType::Welcome) {
    return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                  "service did not send a welcome frame");
  }
  wireops::Welcome welcome;
  if (!wireops::DecodeWelcome(payload, welcome)) {
    return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                  "welcome payload failed decoding");
  }
  session_id_ = welcome.session_id;
  sequence_ = 1;
  server_epoch_ = welcome.epoch;
  server_boot_ = welcome.server_boot;
  server_store_ = welcome.store;
  return Status::Ok();
}

Status ReconciliationClient::Exchange(WireMessageType type, std::string_view payload,
                                      FrameHeader& header, std::string& response) {
  if (socket_ == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed,
                  "client is not connected");
  }
  auto socket = std::static_pointer_cast<net::Socket>(socket_);
  if (socket == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed,
                  "client is not connected");
  }
  const std::string image = EncodeFrame(MakeHeader(type, session_id_, sequence_ + 1), payload);
  const Status sent = net::SendAll(*socket, image.data(), image.size());
  if (!sent.ok()) {
    return sent;
  }
  ++sequence_;
  FrameReader reader(static_cast<std::uint32_t>(limits_.max_wire_payload_bytes));
  std::string frame_payload;
  if (!ReceiveOne(reader, header, frame_payload)) {
    return Status(StatusCode::Indeterminate, ReasonCode::SessionClosed,
                  "service did not answer the request");
  }
  response = std::move(frame_payload);
  return Status::Ok();
}

Status ReconciliationClient::Call(WireOperation operation, std::string_view payload,
                                  std::string& response) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (socket_ == nullptr) {
      return Status(StatusCode::PreconditionFailed, ReasonCode::SessionClosed,
                    "client is not connected");
    }
    wireops::Request request;
    request.operation = operation;
    request.client_boot = client_boot_;
    request.epoch = server_epoch_;
    request.body.assign(payload.data(), payload.size());
    const std::string encoded = wireops::EncodeRequest(request);
    if (encoded.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "request body exceeds the encodable bound");
    }
    FrameHeader header;
    std::string frame_payload;
    const Status exchanged =
        Exchange(WireMessageType::Request, encoded, header, frame_payload);
    if (!exchanged.ok()) {
      return exchanged;
    }
    if (header.type == WireMessageType::Error) {
      wireops::Response error;
      if (!wireops::DecodeResponse(frame_payload, error)) {
        return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                      "error payload failed decoding");
      }
      if (error.reason == ReasonCode::SessionEpochMismatch && attempt == 0) {
        // The service was restarted under a new epoch. Reconnect so that the
        // caller observes the new incarnation rather than a silent retry under
        // the old authority.
        Close();
        auto fresh = std::make_shared<net::Socket>();
        const Status reconnected = net::Connect(host_, port_, *fresh);
        if (!reconnected.ok()) {
          return reconnected;
        }
        auto fresh_wake = std::make_shared<net::WakeupChannel>();
        const Status wake_status = net::WakeupChannel::Create(*fresh_wake);
        if (!wake_status.ok()) {
          return wake_status;
        }
        socket_ = fresh;
        wake_ = fresh_wake;
        const Status handshake = Handshake();
        if (!handshake.ok()) {
          return handshake;
        }
        ++rehandshakes_;
        continue;
      }
      return Status(error.code, error.reason, error.detail);
    }
    if (header.type != WireMessageType::Response) {
      return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                    "service sent an unexpected frame type");
    }
    wireops::Response decoded;
    if (!wireops::DecodeResponse(frame_payload, decoded)) {
      return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                    "response payload failed decoding");
    }
    if (decoded.code != StatusCode::Ok) {
      return Status(decoded.code, decoded.reason, decoded.detail);
    }
    response = std::move(decoded.body);
    return Status::Ok();
  }
  return Status(StatusCode::StaleAuthority, ReasonCode::SessionEpochMismatch,
                "service epoch changed twice while the request was in flight");
}

}  // namespace fabric_reconciliation
}  // namespace summon
