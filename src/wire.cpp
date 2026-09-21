// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/wire.hpp"

#include <cstring>

namespace summon {
namespace fabric_reconciliation {
namespace {

void StoreU16(std::string& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<char>(value & 0xffu);
  out[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

void StoreU32(std::string& out, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xffu);
  }
}

void StoreU64(std::string& out, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xffu);
  }
}

std::uint16_t LoadU16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1])
                                                               << 8));
}

std::uint32_t LoadU32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8u * index);
  }
  return value;
}

std::uint64_t LoadU64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8u * index);
  }
  return value;
}

}  // namespace

const char* ToText(WireMessageType value) noexcept {
  switch (value) {
    case WireMessageType::Invalid: return "INVALID";
    case WireMessageType::Hello: return "HELLO";
    case WireMessageType::Welcome: return "WELCOME";
    case WireMessageType::Request: return "REQUEST";
    case WireMessageType::Response: return "RESPONSE";
    case WireMessageType::Error: return "ERROR";
    case WireMessageType::Ping: return "PING";
    case WireMessageType::Pong: return "PONG";
    case WireMessageType::Goodbye: return "GOODBYE";
  }
  return "INVALID";
}

bool TryParseWireMessageType(std::uint16_t raw, WireMessageType& out) noexcept {
  if (raw < static_cast<std::uint16_t>(WireMessageType::Hello) ||
      raw > static_cast<std::uint16_t>(WireMessageType::Goodbye)) {
    return false;
  }
  out = static_cast<WireMessageType>(raw);
  return true;
}

const char* ToText(WireOperation value) noexcept {
  switch (value) {
    case WireOperation::Invalid: return "INVALID";
    case WireOperation::ActivePolicy: return "ACTIVE_POLICY";
    case WireOperation::PutPolicy: return "PUT_POLICY";
    case WireOperation::CommitIntent: return "COMMIT_INTENT";
    case WireOperation::RecordObservation: return "RECORD_OBSERVATION";
    case WireOperation::Classify: return "CLASSIFY";
    case WireOperation::Plan: return "PLAN";
    case WireOperation::Dispatch: return "DISPATCH";
    case WireOperation::Complete: return "COMPLETE";
    case WireOperation::Verify: return "VERIFY";
    case WireOperation::PutFence: return "PUT_FENCE";
    case WireOperation::ClearFence: return "CLEAR_FENCE";
    case WireOperation::KnownScopes: return "KNOWN_SCOPES";
    case WireOperation::Attempts: return "ATTEMPTS";
    case WireOperation::Outcomes: return "OUTCOMES";
    case WireOperation::BootReport: return "BOOT_REPORT";
    case WireOperation::Compact: return "COMPACT";
  }
  return "INVALID";
}

bool TryParseWireOperation(std::uint16_t raw, WireOperation& out) noexcept {
  if (raw < static_cast<std::uint16_t>(WireOperation::ActivePolicy) ||
      raw > static_cast<std::uint16_t>(WireOperation::Compact)) {
    return false;
  }
  out = static_cast<WireOperation>(raw);
  return true;
}

std::string EncodeFrame(const FrameHeader& header, std::string_view payload) {
  std::string image;
  image.resize(kWireHeaderBytes, '\0');
  std::memcpy(&image[0], kWireMagic, sizeof(kWireMagic));
  StoreU16(image, 4, header.protocol_version);
  StoreU16(image, 6, static_cast<std::uint16_t>(header.type));
  StoreU16(image, 8, header.flags);
  StoreU16(image, 10, 0);
  StoreU32(image, 12, static_cast<std::uint32_t>(payload.size() & 0xffffffffu));
  StoreU64(image, 16, header.session_id);
  StoreU64(image, 24, header.sequence);
  Sha256 hasher;
  hasher.Update(reinterpret_cast<const std::uint8_t*>(image.data()), kWireDigestOffset);
  hasher.Update(payload.data(), payload.size());
  const Sha256Digest digest = hasher.Final();
  std::memcpy(&image[kWireDigestOffset], digest.data(), digest.size());
  image.append(payload.data(), payload.size());
  return image;
}

Status DecodeFrameHeader(const std::uint8_t* data, std::size_t available, std::uint32_t max_payload,
                         FrameHeader& out) {
  if (data == nullptr || available < kWireHeaderBytes) {
    return Status(StatusCode::Indeterminate, ReasonCode::FrameTruncated,
                  "frame header is incomplete");
  }
  if (std::memcmp(data, kWireMagic, sizeof(kWireMagic)) != 0) {
    return Status(StatusCode::Rejected, ReasonCode::FrameMagicInvalid, "frame magic mismatch");
  }
  const std::uint16_t version = LoadU16(data + 4);
  if (version != kWireProtocolVersion) {
    return Status(StatusCode::Unsupported, ReasonCode::FrameVersionUnsupported,
                  "unsupported protocol version");
  }
  WireMessageType type = WireMessageType::Invalid;
  if (!TryParseWireMessageType(LoadU16(data + 6), type)) {
    return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown, "unknown message type");
  }
  const std::uint16_t reserved = LoadU16(data + 10);
  if (reserved != 0) {
    return Status(StatusCode::Rejected, ReasonCode::FrameTypeUnknown,
                  "reserved header bits must be zero");
  }
  const std::uint32_t payload_len = LoadU32(data + 12);
  if (payload_len > max_payload) {
    return Status(StatusCode::LimitExceeded, ReasonCode::FramePayloadTooLarge,
                  "declared payload exceeds the configured maximum");
  }
  out.protocol_version = version;
  out.type = type;
  out.flags = LoadU16(data + 8);
  out.payload_len = payload_len;
  out.session_id = LoadU64(data + 16);
  out.sequence = LoadU64(data + 24);
  return Status::Ok();
}

Status VerifyFrameImage(std::string_view image) {
  if (image.size() < kWireHeaderBytes) {
    return Status(StatusCode::Indeterminate, ReasonCode::FrameTruncated, "frame is incomplete");
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
  if (std::memcmp(bytes, kWireMagic, sizeof(kWireMagic)) != 0) {
    return Status(StatusCode::Rejected, ReasonCode::FrameMagicInvalid, "frame magic mismatch");
  }
  const std::uint32_t payload_len = LoadU32(bytes + 12);
  if (image.size() != kWireHeaderBytes + static_cast<std::size_t>(payload_len)) {
    return Status(StatusCode::Rejected, ReasonCode::FrameTrailingBytes,
                  "frame length does not match its declared payload");
  }
  Sha256 hasher;
  hasher.Update(bytes, kWireDigestOffset);
  hasher.Update(bytes + kWireHeaderBytes, payload_len);
  const Sha256Digest digest = hasher.Final();
  if (std::memcmp(bytes + kWireDigestOffset, digest.data(), digest.size()) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::FrameDigestMismatch,
                  "frame digest mismatch");
  }
  return Status::Ok();
}

void FrameReader::Feed(std::string_view bytes) {
  if (failed_ || bytes.empty()) {
    return;
  }
  buffer_.append(bytes.data(), bytes.size());
}

void FrameReader::Compact() {
  if (offset_ == 0) {
    return;
  }
  buffer_.erase(0, offset_);
  offset_ = 0;
}

bool FrameReader::Next(FrameHeader& header, std::string& payload) {
  if (failed_) {
    return false;
  }
  if (buffer_.size() - offset_ < kWireHeaderBytes) {
    return false;
  }
  const auto* base = reinterpret_cast<const std::uint8_t*>(buffer_.data()) + offset_;
  FrameHeader parsed;
  const Status status = DecodeFrameHeader(base, buffer_.size() - offset_, max_payload_, parsed);
  if (!status.ok()) {
    failed_ = true;
    failure_ = status;
    return false;
  }
  const std::size_t total = kWireHeaderBytes + static_cast<std::size_t>(parsed.payload_len);
  if (buffer_.size() - offset_ < total) {
    return false;
  }
  const std::string_view image(buffer_.data() + offset_, total);
  const Status verified = VerifyFrameImage(image);
  if (!verified.ok()) {
    failed_ = true;
    failure_ = verified;
    return false;
  }
  payload.assign(image.data() + kWireHeaderBytes, parsed.payload_len);
  header = parsed;
  offset_ += total;
  Compact();
  return true;
}

}  // namespace fabric_reconciliation
}  // namespace summon
