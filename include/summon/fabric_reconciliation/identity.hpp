// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities, generations, epochs and process incarnations.
//
// Nothing in this runtime is allowed to compare two differently tagged
// quantities. Matching identity text is never treated as matching generation.

#ifndef SUMMON_FABRIC_RECONCILIATION_IDENTITY_HPP
#define SUMMON_FABRIC_RECONCILIATION_IDENTITY_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/platform.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Maximum accepted length of a textual identity. Longer input is rejected
/// before any allocation proportional to the input is performed.
inline constexpr std::size_t kMaxIdentityLength = 200;

/// Validates textual identity content.
///
/// Accepted: ASCII letters, digits, and the separators '.', '_', '-', '/', ':'.
/// Rejected: empty text, text longer than kMaxIdentityLength, control bytes,
/// backslash, embedded NUL, a leading '/', a leading or trailing separator run
/// that would be ambiguous, and any path-traversal component ("." or "..").
[[nodiscard]] FR_API bool IsValidIdentityText(std::string_view text) noexcept;

/// Tagged, validated, ordered textual identity.
template <typename Tag>
class StringId {
 public:
  StringId() = default;

  /// Parses and validates. Returns nullopt for any invalid input.
  [[nodiscard]] static std::optional<StringId> TryParse(std::string_view text) {
    if (!IsValidIdentityText(text)) {
      return std::nullopt;
    }
    StringId result;
    result.value_.assign(text);
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const std::string& str() const noexcept { return value_; }

  friend bool operator==(const StringId&, const StringId&) = default;
  friend std::strong_ordering operator<=>(const StringId& lhs, const StringId& rhs) {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  std::string value_;
};

struct ScopeIdTag;
struct SubjectIdTag;
struct AttributeKeyTag;
struct ReporterIdTag;
struct PolicyIdTag;
struct DefinitionIdTag;
struct ObservationIdTag;
struct IntentIdTag;
struct FenceIdTag;
struct SessionIdTag;

/// A reconciliation scope: the unit of intent ownership, e.g. "fabric/rack-7".
using ScopeId = StringId<ScopeIdTag>;
/// A reconcilable subject inside a scope, e.g. "node/n17/port/eth0/mtu".
using SubjectId = StringId<SubjectIdTag>;
/// A compared attribute of a subject, e.g. "admin-up".
using AttributeKey = StringId<AttributeKeyTag>;
/// The producer of an observation.
using ReporterId = StringId<ReporterIdTag>;
/// A named policy document.
using PolicyId = StringId<PolicyIdTag>;
/// A named intent definition.
using DefinitionId = StringId<DefinitionIdTag>;
/// The identity of one concrete observation document.
using ObservationId = StringId<ObservationIdTag>;
/// The identity of one concrete intent document.
using IntentId = StringId<IntentIdTag>;
/// The identity of one fence entry.
using FenceId = StringId<FenceIdTag>;

/// Tagged monotonic counter. Distinct tags make distinct quantities
/// non-interchangeable, including at compile time.
template <typename Tag, typename Rep = std::uint64_t>
class Counter {
 public:
  using rep_type = Rep;

  constexpr Counter() = default;
  constexpr explicit Counter(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  /// Returns the next counter value. The caller must already have proven that
  /// the increment does not wrap; Next() itself is checked and returns false
  /// instead of wrapping.
  [[nodiscard]] constexpr bool TryNext(Counter& out) const noexcept {
    if (value_ == (std::numeric_limits<Rep>::max)()) {
      return false;
    }
    out = Counter(static_cast<Rep>(value_ + 1));
    return true;
  }

  friend constexpr bool operator==(Counter, Counter) = default;
  friend constexpr std::strong_ordering operator<=>(Counter lhs, Counter rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  Rep value_{0};
};

struct IntentGenerationTag;
struct ObservationGenerationTag;
struct CoordinatorEpochTag;
struct PolicyVersionTag;
struct SchemaVersionTag;
struct RecordSequenceTag;
struct AttemptIdTag;
struct SessionEpochTag;

/// Monotonic generation of an intent document within one scope. Every
/// authority-bearing decision names the exact intent generation it rests on.
using IntentGeneration = Counter<IntentGenerationTag>;
/// Monotonic generation of an observation stream within one (scope, reporter).
using ObservationGeneration = Counter<ObservationGenerationTag>;
/// Coordinator epoch. Advances on every process incarnation; fences every
/// pre-restart grant, attempt and completion.
using CoordinatorEpoch = Counter<CoordinatorEpochTag>;
/// Monotonic version of the active reconciliation policy.
using PolicyVersion = Counter<PolicyVersionTag>;
/// Monotonic durable record sequence. A regression is a hard integrity error.
using RecordSequence = Counter<RecordSequenceTag>;
/// Monotonic attempt identity within one (epoch, boot) authority.
using AttemptId = Counter<AttemptIdTag>;

/// 128-bit process incarnation identity. Distinct across processes, restarts
/// and rapid relaunches.
class FR_API BootId {
 public:
  BootId() = default;

  /// Draws from the OS entropy source mixed with pid, wall clock and a process
  /// local counter. Uniqueness, not unpredictability, is required.
  [[nodiscard]] static BootId Generate() noexcept;

  [[nodiscard]] static BootId FromBytes(const std::array<std::uint8_t, 16>& bytes) noexcept;

  [[nodiscard]] const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string ToHex() const;

  friend bool operator==(const BootId&, const BootId&) = default;
  friend std::strong_ordering operator<=>(const BootId& lhs, const BootId& rhs) noexcept {
    return lhs.bytes_ <=> rhs.bytes_;
  }

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

/// A globally unique durable-store identity. Guards against opening a snapshot
/// and a journal that belong to different stores.
class FR_API StoreId {
 public:
  StoreId() = default;

  [[nodiscard]] static StoreId Generate() noexcept;
  [[nodiscard]] static StoreId FromBytes(const std::array<std::uint8_t, 16>& bytes) noexcept;

  [[nodiscard]] const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string ToHex() const;
  [[nodiscard]] bool IsZero() const noexcept;

  friend bool operator==(const StoreId&, const StoreId&) = default;

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

/// Wall-clock milliseconds since the Unix epoch, as reported by the host.
/// Used only for evidence timestamps, never as the sole proof of freshness.
using UnixMillis = std::int64_t;

/// Reads the host wall clock. Values are not monotonic and may regress.
[[nodiscard]] FR_API UnixMillis WallClockMillis() noexcept;

/// Monotonic milliseconds since an unspecified origin. Never persisted.
[[nodiscard]] FR_API std::uint64_t MonotonicMillis() noexcept;

/// Current process identifier, or 0 when unavailable.
[[nodiscard]] FR_API std::uint64_t CurrentProcessId() noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_IDENTITY_HPP
