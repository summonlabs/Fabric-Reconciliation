// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Portability helpers: symbol visibility and checked integer arithmetic.
// No wraparound is ever permitted on an authority-bearing quantity.

#ifndef SUMMON_FABRIC_RECONCILIATION_PLATFORM_HPP
#define SUMMON_FABRIC_RECONCILIATION_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_WIN32) && defined(FABRIC_RECONCILIATION_SHARED)
#if defined(FABRIC_RECONCILIATION_BUILDING_LIBRARY)
#define FR_API __declspec(dllexport)
#else
#define FR_API __declspec(dllimport)
#endif
#else
#define FR_API
#endif

namespace summon {
namespace fabric_reconciliation {

/// True when adding @p a and @p b as unsigned 64-bit values would wrap.
[[nodiscard]] constexpr bool AddWouldOverflow(std::uint64_t a, std::uint64_t b) noexcept {
  return a > (std::numeric_limits<std::uint64_t>::max)() - b;
}

/// True when multiplying @p a and @p b as unsigned 64-bit values would wrap.
[[nodiscard]] constexpr bool MulWouldOverflow(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return false;
  }
  return a > (std::numeric_limits<std::uint64_t>::max)() / b;
}

/// True when multiplying @p a and @p b as @c std::size_t values would wrap.
[[nodiscard]] constexpr bool SizeMulWouldOverflow(std::size_t a, std::size_t b) noexcept {
  return MulWouldOverflow(static_cast<std::uint64_t>(a), static_cast<std::uint64_t>(b));
}

/// Checked rounding-up alignment of @p value to @p alignment.
/// Returns false, and leaves @p out untouched, when the computation would wrap.
[[nodiscard]] constexpr bool AlignUpChecked(std::uint64_t value, std::uint64_t alignment,
                                            std::uint64_t& out) noexcept {
  if (alignment == 0) {
    return false;
  }
  const std::uint64_t remainder = value % alignment;
  if (remainder == 0) {
    out = value;
    return true;
  }
  const std::uint64_t delta = alignment - remainder;
  if (AddWouldOverflow(value, delta)) {
    return false;
  }
  out = value + delta;
  return true;
}

/// Narrowing helper that makes every intentional integer narrowing explicit and
/// therefore visible to the strict-warning build.
template <typename To, typename From>
[[nodiscard]] constexpr To Narrow(From value) noexcept {
  static_assert(std::is_integral_v<To>, "Narrow target must be integral");
  static_assert(std::is_integral_v<From>, "Narrow source must be integral");
  return static_cast<To>(value);
}

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_PLATFORM_HPP
