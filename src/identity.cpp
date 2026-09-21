// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/identity.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace summon {
namespace fabric_reconciliation {
namespace {

bool IsAlphaNumeric(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9');
}

bool IsSeparator(char character) noexcept {
  return character == '.' || character == '_' || character == '-' || character == '/' ||
         character == ':';
}

void FillEntropy(std::uint8_t* out, std::size_t size) noexcept {
  std::random_device device;
  std::size_t produced = 0;
  while (produced < size) {
    const unsigned value = device();
    out[produced] = static_cast<std::uint8_t>(value & 0xffu);
    ++produced;
    if (produced < size) {
      out[produced] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
      ++produced;
    }
  }
  // Mix in process, thread and time entropy so that a deterministic
  // random_device cannot make two incarnations collide.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t mix = counter.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t stamp = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t process = CurrentProcessId();
  std::uint64_t words[3] = {mix, stamp, process};
  for (std::size_t index = 0; index < size; ++index) {
    const std::size_t word_index = index % 3;
    const unsigned shift = static_cast<unsigned>((index / 3) % 8) * 8u;
    out[index] ^= static_cast<std::uint8_t>((words[word_index] >> shift) & 0xffu);
  }
}

std::string HexOf(const std::uint8_t* bytes, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string result;
  result.resize(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    result[index * 2] = kDigits[(bytes[index] >> 4) & 0x0fu];
    result[index * 2 + 1] = kDigits[bytes[index] & 0x0fu];
  }
  return result;
}

}  // namespace

bool IsValidIdentityText(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxIdentityLength) {
    return false;
  }
  if (text.front() == '/' || text.front() == '.' || text.back() == '/' || text.back() == '.') {
    return false;
  }
  char previous = '\0';
  for (const char character : text) {
    if (character == '\0' || character == '\\') {
      return false;
    }
    const unsigned char raw = static_cast<unsigned char>(character);
    if (raw < 0x20u || raw == 0x7fu) {
      return false;
    }
    const bool separator = IsSeparator(character);
    if (!separator && !IsAlphaNumeric(character)) {
      return false;
    }
    if (separator && IsSeparator(previous)) {
      // Adjacent separators are ambiguous with empty path components.
      return false;
    }
    previous = character;
  }
  // Reject any path-traversal component outright.
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('/', start);
    const std::size_t stop = (end == std::string_view::npos) ? text.size() : end;
    const std::string_view component = text.substr(start, stop - start);
    if (component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

BootId BootId::FromBytes(const std::array<std::uint8_t, 16>& bytes) noexcept {
  BootId result;
  result.bytes_ = bytes;
  return result;
}

BootId BootId::Generate() noexcept {
  BootId result;
  FillEntropy(result.bytes_.data(), result.bytes_.size());
  return result;
}

std::string BootId::ToHex() const { return HexOf(bytes_.data(), bytes_.size()); }

StoreId StoreId::FromBytes(const std::array<std::uint8_t, 16>& bytes) noexcept {
  StoreId result;
  result.bytes_ = bytes;
  return result;
}

StoreId StoreId::Generate() noexcept {
  StoreId result;
  FillEntropy(result.bytes_.data(), result.bytes_.size());
  return result;
}

std::string StoreId::ToHex() const { return HexOf(bytes_.data(), bytes_.size()); }

bool StoreId::IsZero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

UnixMillis WallClockMillis() noexcept {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::uint64_t MonotonicMillis() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint64_t CurrentProcessId() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace fabric_reconciliation
}  // namespace summon
