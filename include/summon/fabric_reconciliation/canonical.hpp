// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic canonical byte encoding.
//
// Every quantity that can appear in a durable record, a wire frame or a plan
// digest is written through this writer. The encoding is fixed-width
// little-endian with explicit length prefixes for variable-length text, so the
// byte stream for a value is a pure function of the value and of nothing else
// (no container iteration order, no locale, no pointer values).

#ifndef SUMMON_FABRIC_RECONCILIATION_CANONICAL_HPP
#define SUMMON_FABRIC_RECONCILIATION_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/version.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Maximum size of a single canonical buffer that the runtime will build.
/// Larger documents must be split by the caller; exceeding the bound is an
/// explicit refusal, never a silent truncation.
inline constexpr std::size_t kMaxCanonicalBytes = 64u * 1024u * 1024u;

/// Append-only canonical encoder with a hard size ceiling.
class FR_API CanonicalWriter {
 public:
  CanonicalWriter() = default;
  explicit CanonicalWriter(std::size_t reserve);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const std::string& buffer() const noexcept { return buffer_; }

  void PutU8(std::uint8_t value) noexcept;
  void PutU16(std::uint16_t value) noexcept;
  void PutU32(std::uint32_t value) noexcept;
  void PutU64(std::uint64_t value) noexcept;
  void PutI64(std::int64_t value) noexcept;
  void PutBool(bool value) noexcept;
  /// Length-prefixed bytes: u32 length followed by the raw bytes.
  void PutBytes(std::string_view bytes) noexcept;
  void PutDigest(const Sha256Digest& digest) noexcept;

  [[nodiscard]] Sha256Digest Digest() const noexcept;

 private:
  void Reserve(std::size_t additional) noexcept;

  std::string buffer_;
  bool ok_{true};
};

/// Total, sticky-failure canonical decoder.
///
/// A read that runs past the end, or that is asked for a length beyond the
/// remaining buffer, fails and puts the reader into a permanently failed
/// state. Callers must check ok() before trusting any decoded value.
class FR_API CanonicalReader {
 public:
  CanonicalReader(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size), offset_(0) {}
  explicit CanonicalReader(std::string_view bytes) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(bytes.data())), size_(bytes.size()), offset_(0) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? size_ - offset_ : 0; }

  [[nodiscard]] bool ReadU8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool ReadU16(std::uint16_t& out) noexcept;
  [[nodiscard]] bool ReadU32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool ReadU64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool ReadI64(std::int64_t& out) noexcept;
  [[nodiscard]] bool ReadBool(bool& out) noexcept;
  [[nodiscard]] bool ReadBytes(std::string& out) noexcept;
  [[nodiscard]] bool ReadDigest(Sha256Digest& out) noexcept;

  /// True when every byte has been consumed. Trailing bytes are a hard error
  /// for canonical payloads.
  [[nodiscard]] bool AtEnd() const noexcept { return ok_ && offset_ == size_; }

 private:
  void Fail() noexcept { ok_ = false; }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_;
  bool ok_{true};
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_CANONICAL_HPP
