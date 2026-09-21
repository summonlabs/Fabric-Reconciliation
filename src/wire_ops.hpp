// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal envelope encoding shared by the service and its client. Every
// request is bound to the session identity the connection established: the
// client boot identity recorded at handshake and the coordinator epoch the
// client believes is current. A request that carries a different identity or a
// different epoch is refused before any engine call happens.

#ifndef SUMMON_FABRIC_REOCONCILIATION_WIRE_OPS_HPP
#define SUMMON_FABRIC_REOCONCILIATION_WIRE_OPS_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/authority.hpp"
#include "summon/fabric_reconciliation/wire.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace wireops {

inline void PutBoot(CanonicalWriter& writer, const BootId& boot) noexcept {
  writer.PutBytes(std::string_view(reinterpret_cast<const char*>(boot.bytes().data()),
                                   boot.bytes().size()));
}

inline bool GetBoot(CanonicalReader& reader, BootId& out) {
  std::string bytes;
  if (!reader.ReadBytes(bytes) || bytes.size() != 16) {
    return false;
  }
  std::array<std::uint8_t, 16> raw{};
  for (std::size_t index = 0; index < 16; ++index) {
    raw[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[index]));
  }
  out = BootId::FromBytes(raw);
  return true;
}

inline void PutStore(CanonicalWriter& writer, const StoreId& store) noexcept {
  writer.PutBytes(std::string_view(reinterpret_cast<const char*>(store.bytes().data()),
                                   store.bytes().size()));
}

inline bool GetStore(CanonicalReader& reader, StoreId& out) {
  std::string bytes;
  if (!reader.ReadBytes(bytes) || bytes.size() != 16) {
    return false;
  }
  std::array<std::uint8_t, 16> raw{};
  for (std::size_t index = 0; index < 16; ++index) {
    raw[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[index]));
  }
  out = StoreId::FromBytes(raw);
  return true;
}

inline std::string EncodeHello(const BootId& client_boot, const std::string& nonce) {
  CanonicalWriter writer;
  writer.PutU16(kWireProtocolVersion);
  PutBoot(writer, client_boot);
  writer.PutBytes(nonce);
  return writer.ok() ? writer.buffer() : std::string();
}

inline bool DecodeHello(std::string_view payload, BootId& client_boot, std::string& nonce) {
  CanonicalReader reader(payload);
  std::uint16_t version = 0;
  if (!reader.ReadU16(version) || version != kWireProtocolVersion) {
    return false;
  }
  if (!GetBoot(reader, client_boot) || !reader.ReadBytes(nonce)) {
    return false;
  }
  return reader.AtEnd();
}

struct Welcome {
  std::uint16_t protocol_version{kWireProtocolVersion};
  std::uint64_t session_id{0};
  CoordinatorEpoch epoch;
  BootId server_boot;
  StoreId store;
  std::string nonce;
  std::uint32_t max_payload{0};
};

inline std::string EncodeWelcome(const Welcome& welcome) {
  CanonicalWriter writer;
  writer.PutU16(welcome.protocol_version);
  writer.PutU64(welcome.session_id);
  writer.PutU64(welcome.epoch.value());
  PutBoot(writer, welcome.server_boot);
  PutStore(writer, welcome.store);
  writer.PutBytes(welcome.nonce);
  writer.PutU32(welcome.max_payload);
  return writer.ok() ? writer.buffer() : std::string();
}

inline bool DecodeWelcome(std::string_view payload, Welcome& out) {
  CanonicalReader reader(payload);
  std::uint64_t session_id = 0;
  std::uint64_t epoch = 0;
  if (!reader.ReadU16(out.protocol_version) || out.protocol_version != kWireProtocolVersion) {
    return false;
  }
  if (!reader.ReadU64(session_id) || !reader.ReadU64(epoch)) {
    return false;
  }
  if (!GetBoot(reader, out.server_boot) || !GetStore(reader, out.store)) {
    return false;
  }
  out.session_id = session_id;
  out.epoch = CoordinatorEpoch(epoch);
  if (!reader.ReadBytes(out.nonce) || !reader.ReadU32(out.max_payload)) {
    return false;
  }
  return reader.AtEnd();
}

struct Request {
  WireOperation operation{WireOperation::Invalid};
  BootId client_boot;
  CoordinatorEpoch epoch;
  std::string body;
};

inline std::string EncodeRequest(const Request& request) {
  CanonicalWriter writer;
  writer.PutU16(static_cast<std::uint16_t>(request.operation));
  PutBoot(writer, request.client_boot);
  writer.PutU64(request.epoch.value());
  writer.PutBytes(request.body);
  return writer.ok() ? writer.buffer() : std::string();
}

inline bool DecodeRequest(std::string_view payload, Request& out) {
  CanonicalReader reader(payload);
  std::uint16_t raw = 0;
  if (!reader.ReadU16(raw) || !TryParseWireOperation(raw, out.operation)) {
    return false;
  }
  std::uint64_t epoch = 0;
  if (!GetBoot(reader, out.client_boot) || !reader.ReadU64(epoch)) {
    return false;
  }
  out.epoch = CoordinatorEpoch(epoch);
  if (!reader.ReadBytes(out.body)) {
    return false;
  }
  return reader.AtEnd();
}

struct Response {
  StatusCode code{StatusCode::Ok};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
  std::string body;
};

inline std::string EncodeResponse(const Response& response) {
  CanonicalWriter writer;
  writer.PutU16(static_cast<std::uint16_t>(response.code));
  writer.PutU16(static_cast<std::uint16_t>(response.reason));
  writer.PutBytes(response.detail);
  writer.PutBytes(response.body);
  return writer.ok() ? writer.buffer() : std::string();
}

inline bool DecodeResponse(std::string_view payload, Response& out) {
  CanonicalReader reader(payload);
  std::uint16_t code = 0;
  std::uint16_t reason = 0;
  if (!reader.ReadU16(code) || !reader.ReadU16(reason)) {
    return false;
  }
  if (code > static_cast<std::uint16_t>(StatusCode::InternalError) ||
      reason > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored)) {
    return false;
  }
  out.code = static_cast<StatusCode>(code);
  out.reason = static_cast<ReasonCode>(reason);
  if (!reader.ReadBytes(out.detail) || !reader.ReadBytes(out.body)) {
    return false;
  }
  return reader.AtEnd();
}

}  // namespace wireops
}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_REOCONCILIATION_WIRE_OPS_HPP
