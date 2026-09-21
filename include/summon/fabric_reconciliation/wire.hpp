// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded framed protocol.
//
// Every frame is self-describing and integrity-checked:
//   [0,4)   magic 'F','R','P','1'
//   [4,6)   protocol version
//   [6,8)   message type
//   [8,10)  flags
//   [10,12) reserved, must be zero
//   [12,16) payload length
//   [16,24) session id
//   [24,32) sequence
//   [32,64) SHA-256 over bytes [0,32) and the payload
//
// The declared payload length is checked against the configured maximum before
// any allocation proportional to it is attempted. Decoding is total and its
// failures are sticky: once a reader has rejected a frame it refuses every
// later frame on that stream.
//
// Trust boundary: this transport provides integrity against corruption and
// against cross-session confusion. It does not provide authentication,
// confidentiality or protection against an active on-path attacker, and the
// runtime does not claim that it does.

#ifndef SUMMON_FABRIC_RECONCILIATION_WIRE_HPP
#define SUMMON_FABRIC_RECONCILIATION_WIRE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/version.hpp"

namespace summon {
namespace fabric_reconciliation {

inline constexpr std::size_t kWireHeaderBytes = 64;
inline constexpr std::size_t kWireDigestOffset = 32;
inline constexpr char kWireMagic[4] = {'F', 'R', 'P', '1'};

enum class WireMessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  Welcome = 2,
  Request = 3,
  Response = 4,
  Error = 5,
  Ping = 6,
  Pong = 7,
  Goodbye = 8,
};

[[nodiscard]] FR_API const char* ToText(WireMessageType value) noexcept;
[[nodiscard]] FR_API bool TryParseWireMessageType(std::uint16_t raw, WireMessageType& out) noexcept;

/// The operation carried by a Request frame.
enum class WireOperation : std::uint16_t {
  Invalid = 0,
  ActivePolicy = 1,
  PutPolicy = 2,
  CommitIntent = 3,
  RecordObservation = 4,
  Classify = 5,
  Plan = 6,
  Dispatch = 7,
  Complete = 8,
  Verify = 9,
  PutFence = 10,
  ClearFence = 11,
  KnownScopes = 12,
  Attempts = 13,
  Outcomes = 14,
  BootReport = 15,
  Compact = 16,
};

[[nodiscard]] FR_API const char* ToText(WireOperation value) noexcept;
[[nodiscard]] FR_API bool TryParseWireOperation(std::uint16_t raw, WireOperation& out) noexcept;

struct FR_API FrameHeader {
  std::uint16_t protocol_version{kWireProtocolVersion};
  WireMessageType type{WireMessageType::Invalid};
  std::uint16_t flags{0};
  std::uint32_t payload_len{0};
  std::uint64_t session_id{0};
  std::uint64_t sequence{0};
};

[[nodiscard]] FR_API std::string EncodeFrame(const FrameHeader& header, std::string_view payload);

/// Parses a frame header from a buffer that must contain at least
/// kWireHeaderBytes. Rejects bad magic, unsupported version, unknown reserved
/// bits and a declared payload length above @p max_payload.
[[nodiscard]] FR_API Status DecodeFrameHeader(const std::uint8_t* data, std::size_t available,
                                              std::uint32_t max_payload, FrameHeader& out);

/// Verifies the digest of a complete frame image and rejects trailing bytes.
[[nodiscard]] FR_API Status VerifyFrameImage(std::string_view image);

/// Incremental, sticky-failure frame reader over a byte stream.
class FR_API FrameReader {
 public:
  explicit FrameReader(std::uint32_t max_payload) : max_payload_(max_payload) {}

  void Feed(std::string_view bytes);

  /// Extracts the next complete frame. Returns false when more bytes are
  /// needed, or when the reader has permanently failed; check failed().
  [[nodiscard]] bool Next(FrameHeader& header, std::string& payload);

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const Status& failure() const noexcept { return failure_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - offset_; }

 private:
  void Compact();

  std::string buffer_;
  std::size_t offset_{0};
  std::uint32_t max_payload_;
  bool failed_{false};
  Status failure_;
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_WIRE_HPP
