// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Integrity digests.
//
// SHA-256 is implemented here purely as a corruption and torn-write detector
// for durable records and wire frames. It is not authentication: the runtime
// does not claim an authenticated or confidential transport. See the README
// sections "Trust boundary" and "Evidence matrix".

#ifndef SUMMON_FABRIC_RECONCILIATION_HASH_HPP
#define SUMMON_FABRIC_RECONCILIATION_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/platform.hpp"

namespace summon {
namespace fabric_reconciliation {

inline constexpr std::size_t kSha256Bytes = 32;

using Sha256Digest = std::array<std::uint8_t, kSha256Bytes>;

/// Streaming SHA-256. Deterministic, allocation-free, total for any input.
class FR_API Sha256 {
 public:
  Sha256() noexcept;

  void Update(const void* data, std::size_t size) noexcept;
  void Update(std::string_view text) noexcept;

  /// Finalises and returns the digest. The object must not be reused for
  /// further updates without a fresh construction.
  [[nodiscard]] Sha256Digest Final() noexcept;

 private:
  void Compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::uint8_t buffer_[64];
  std::size_t buffered_;
};

[[nodiscard]] FR_API Sha256Digest Sha256Of(const void* data, std::size_t size) noexcept;
[[nodiscard]] FR_API Sha256Digest Sha256Of(std::string_view text) noexcept;

/// Lowercase hex rendering, always exactly 64 characters.
[[nodiscard]] FR_API std::string ToHex(const Sha256Digest& digest);

/// Strict hex parsing: exactly 64 hex characters.
/// Any other input, including a correct prefix with trailing bytes, fails.
[[nodiscard]] FR_API bool TryParseHexDigest(std::string_view text, Sha256Digest& out) noexcept;

/// Fast non-cryptographic order-independent mix used only for in-memory hash
/// containers. Never used for durable integrity.
[[nodiscard]] FR_API std::uint64_t Fnv1a64(std::string_view text) noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_HASH_HPP
