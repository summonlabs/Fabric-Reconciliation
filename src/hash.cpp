// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/hash.hpp"

#include <cstring>

namespace summon {
namespace fabric_reconciliation {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t RotateRight(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

constexpr std::uint32_t Choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

constexpr std::uint32_t Majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t BigSigma0(std::uint32_t x) noexcept {
  return RotateRight(x, 2) ^ RotateRight(x, 13) ^ RotateRight(x, 22);
}

constexpr std::uint32_t BigSigma1(std::uint32_t x) noexcept {
  return RotateRight(x, 6) ^ RotateRight(x, 11) ^ RotateRight(x, 25);
}

constexpr std::uint32_t SmallSigma0(std::uint32_t x) noexcept {
  return RotateRight(x, 7) ^ RotateRight(x, 18) ^ (x >> 3);
}

constexpr std::uint32_t SmallSigma1(std::uint32_t x) noexcept {
  return RotateRight(x, 17) ^ RotateRight(x, 19) ^ (x >> 10);
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
             0x1f83d9abu, 0x5be0cd19u},
      bit_length_(0),
      buffer_{},
      buffered_(0) {}

void Sha256::Compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    schedule[index] = SmallSigma1(schedule[index - 2]) + schedule[index - 7] +
                      SmallSigma0(schedule[index - 15]) + schedule[index - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t temp1 = h + BigSigma1(e) + Choose(e, f, g) + kRoundConstants[index] +
                                schedule[index];
    const std::uint32_t temp2 = BigSigma0(a) + Majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const void* data, std::size_t size) noexcept {
  if (size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bit_length_ += static_cast<std::uint64_t>(size) * 8u;

  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = (size < need) ? size : need;
    std::memcpy(buffer_ + buffered_, bytes, take);
    buffered_ += take;
    if (buffered_ == 64) {
      Compress(buffer_);
      buffered_ = 0;
    }
    if (take == size) {
      return;
    }
    bytes += take;
    size -= take;
  }

  while (size >= 64) {
    Compress(bytes);
    bytes += 64;
    size -= 64;
  }

  if (size > 0) {
    std::memcpy(buffer_, bytes, size);
    buffered_ = size;
  }
}

void Sha256::Update(std::string_view text) noexcept {
  Update(text.data(), text.size());
}

Sha256Digest Sha256::Final() noexcept {
  const std::uint64_t total_bits = bit_length_;
  const std::uint8_t padding = 0x80u;
  Update(&padding, 1);
  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    Update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((total_bits >> (56u - (8u * index))) & 0xffu);
  }
  Update(length_bytes, 8);

  Sha256Digest digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xffu);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xffu);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xffu);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xffu);
  }
  return digest;
}

Sha256Digest Sha256Of(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.Update(data, size);
  return hasher.Final();
}

Sha256Digest Sha256Of(std::string_view text) noexcept {
  return Sha256Of(text.data(), text.size());
}

std::string ToHex(const Sha256Digest& digest) {
  static const char kDigits[] = "0123456789abcdef";
  std::string result;
  result.resize(kSha256Bytes * 2);
  for (std::size_t index = 0; index < kSha256Bytes; ++index) {
    const std::uint8_t byte = digest[index];
    result[index * 2] = kDigits[(byte >> 4) & 0x0fu];
    result[index * 2 + 1] = kDigits[byte & 0x0fu];
  }
  return result;
}

bool TryParseHexDigest(std::string_view text, Sha256Digest& out) noexcept {
  if (text.size() != kSha256Bytes * 2) {
    return false;
  }
  Sha256Digest parsed{};
  for (std::size_t index = 0; index < kSha256Bytes; ++index) {
    unsigned value = 0;
    for (std::size_t half = 0; half < 2; ++half) {
      const char character = text[index * 2 + half];
      unsigned nibble = 0;
      if (character >= '0' && character <= '9') {
        nibble = static_cast<unsigned>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        nibble = static_cast<unsigned>(character - 'a') + 10u;
      } else if (character >= 'A' && character <= 'F') {
        nibble = static_cast<unsigned>(character - 'A') + 10u;
      } else {
        return false;
      }
      value = (value << 4) | nibble;
    }
    parsed[index] = static_cast<std::uint8_t>(value);
  }
  out = parsed;
  return true;
}

std::uint64_t Fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char character : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace fabric_reconciliation
}  // namespace summon
