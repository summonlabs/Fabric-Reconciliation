// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/canonical.hpp"

#include <cstring>

namespace summon {
namespace fabric_reconciliation {

CanonicalWriter::CanonicalWriter(std::size_t reserve) {
  if (reserve > kMaxCanonicalBytes) {
    ok_ = false;
    return;
  }
  buffer_.reserve(reserve);
}

void CanonicalWriter::Reserve(std::size_t additional) noexcept {
  if (!ok_) {
    return;
  }
  if (additional > kMaxCanonicalBytes || buffer_.size() > kMaxCanonicalBytes - additional) {
    ok_ = false;
    return;
  }
  try {
    buffer_.reserve(buffer_.size() + additional);
  } catch (...) {
    ok_ = false;
  }
}

void CanonicalWriter::PutU8(std::uint8_t value) noexcept {
  Reserve(1);
  if (ok_) {
    buffer_.push_back(static_cast<char>(value));
  }
}

void CanonicalWriter::PutU16(std::uint16_t value) noexcept {
  PutU8(static_cast<std::uint8_t>(value & 0xffu));
  PutU8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void CanonicalWriter::PutU32(std::uint32_t value) noexcept {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    PutU8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void CanonicalWriter::PutU64(std::uint64_t value) noexcept {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    PutU8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void CanonicalWriter::PutI64(std::int64_t value) noexcept {
  PutU64(static_cast<std::uint64_t>(value));
}

void CanonicalWriter::PutBool(bool value) noexcept { PutU8(value ? 1u : 0u); }

void CanonicalWriter::PutBytes(std::string_view bytes) noexcept {
  const std::uint64_t length = static_cast<std::uint64_t>(bytes.size());
  if (length > 0xffffffffull) {
    ok_ = false;
    return;
  }
  PutU32(static_cast<std::uint32_t>(length));
  if (!ok_) {
    return;
  }
  Reserve(bytes.size());
  if (ok_) {
    buffer_.append(bytes.data(), bytes.size());
  }
}

void CanonicalWriter::PutDigest(const Sha256Digest& digest) noexcept {
  Reserve(digest.size());
  if (!ok_) {
    return;
  }
  buffer_.append(reinterpret_cast<const char*>(digest.data()), digest.size());
}

Sha256Digest CanonicalWriter::Digest() const noexcept {
  return Sha256Of(buffer_.data(), buffer_.size());
}

bool CanonicalReader::ReadU8(std::uint8_t& out) noexcept {
  if (!ok_ || offset_ + 1 > size_) {
    Fail();
    return false;
  }
  out = data_[offset_];
  ++offset_;
  return true;
}

bool CanonicalReader::ReadU16(std::uint16_t& out) noexcept {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (!ReadU8(low) || !ReadU8(high)) {
    return false;
  }
  out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                   static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8));
  return true;
}

bool CanonicalReader::ReadU32(std::uint32_t& out) noexcept {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    std::uint8_t byte = 0;
    if (!ReadU8(byte)) {
      return false;
    }
    value |= static_cast<std::uint32_t>(byte) << shift;
  }
  out = value;
  return true;
}

bool CanonicalReader::ReadU64(std::uint64_t& out) noexcept {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    std::uint8_t byte = 0;
    if (!ReadU8(byte)) {
      return false;
    }
    value |= static_cast<std::uint64_t>(byte) << shift;
  }
  out = value;
  return true;
}

bool CanonicalReader::ReadI64(std::int64_t& out) noexcept {
  std::uint64_t raw = 0;
  if (!ReadU64(raw)) {
    return false;
  }
  out = static_cast<std::int64_t>(raw);
  return true;
}

bool CanonicalReader::ReadBool(bool& out) noexcept {
  std::uint8_t raw = 0;
  if (!ReadU8(raw)) {
    return false;
  }
  out = (raw != 0);
  return true;
}

bool CanonicalReader::ReadBytes(std::string& out) noexcept {
  std::uint32_t length = 0;
  if (!ReadU32(length)) {
    return false;
  }
  const std::size_t needed = static_cast<std::size_t>(length);
  if (needed > remaining()) {
    Fail();
    return false;
  }
  if (needed > kMaxCanonicalBytes) {
    Fail();
    return false;
  }
  try {
    out.assign(reinterpret_cast<const char*>(data_ + offset_), needed);
  } catch (...) {
    Fail();
    return false;
  }
  offset_ += needed;
  return true;
}

bool CanonicalReader::ReadDigest(Sha256Digest& out) noexcept {
  if (remaining() < kSha256Bytes) {
    Fail();
    return false;
  }
  std::memcpy(out.data(), data_ + offset_, kSha256Bytes);
  offset_ += kSha256Bytes;
  return true;
}

}  // namespace fabric_reconciliation
}  // namespace summon
