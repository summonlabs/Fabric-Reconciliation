// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Attribute values and subject state.
//
// Comparison is kind-sensitive: two values of different kinds are never
// silently coerced, they are reported as not comparable, which surfaces as an
// explicit UNKNOWN or MISMATCH depending on the surrounding coverage facts.

#ifndef SUMMON_FABRIC_RECONCILIATION_VALUE_HPP
#define SUMMON_FABRIC_RECONCILIATION_VALUE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/platform.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Maximum accepted length of a Text or Token attribute value.
inline constexpr std::size_t kMaxAttributeTextLength = 4096;

enum class AttributeKind : std::uint8_t {
  Invalid = 0,
  /// The attribute is explicitly not present.
  Absent = 1,
  Boolean = 2,
  Integer = 3,
  Unsigned = 4,
  /// Opaque free text, compared byte for byte.
  Text = 5,
  /// A closed token from a controlled vocabulary, compared byte for byte.
  Token = 6,
};

[[nodiscard]] FR_API const char* ToText(AttributeKind value) noexcept;
[[nodiscard]] FR_API bool TryParseAttributeKind(const char* text, AttributeKind& out) noexcept;

/// One compared attribute value. Exactly one payload is meaningful, selected by
/// kind(); the invariant is maintained by the constructors.
class FR_API AttributeValue {
 public:
  AttributeValue() = default;

  [[nodiscard]] static AttributeValue Absent();
  [[nodiscard]] static AttributeValue Boolean(bool value);
  [[nodiscard]] static AttributeValue Integer(std::int64_t value);
  [[nodiscard]] static AttributeValue Unsigned(std::uint64_t value);
  [[nodiscard]] static AttributeValue Text(std::string value);
  [[nodiscard]] static AttributeValue Token(std::string value);

  [[nodiscard]] AttributeKind kind() const noexcept { return kind_; }

  [[nodiscard]] bool bool_value() const noexcept { return bool_; }
  [[nodiscard]] std::int64_t int_value() const noexcept { return int_; }
  [[nodiscard]] std::uint64_t uint_value() const noexcept { return uint_; }
  [[nodiscard]] const std::string& text_value() const noexcept { return text_; }

  /// Structural equality. Values of different kinds are never equal, and are
  /// never coerced into one another.
  friend bool operator==(const AttributeValue&, const AttributeValue&) = default;

  /// True when both values have the same kind and can therefore be compared
  /// for reconciliation purposes.
  [[nodiscard]] bool ComparableWith(const AttributeValue& other) const noexcept {
    return kind_ == other.kind_ && kind_ != AttributeKind::Invalid;
  }

  void Encode(CanonicalWriter& writer) const noexcept;

  /// Decodes one value. Fails, without allocating beyond the encoded length,
  /// on an unknown kind, an over-long text payload or trailing bytes.
  [[nodiscard]] static bool Decode(CanonicalReader& reader, AttributeValue& out);

  /// Canonical, human-oriented rendering used by explain output.
  [[nodiscard]] std::string Render() const;

  /// Validates internal consistency, including the text length bound.
  [[nodiscard]] bool IsValid() const noexcept;

 private:
  AttributeKind kind_{AttributeKind::Invalid};
  bool bool_{false};
  std::int64_t int_{0};
  std::uint64_t uint_{0};
  std::string text_;
};

/// Ordered attribute map. std::map gives a deterministic iteration order that
/// is identical on every platform and independent of insertion order.
using SubjectState = std::map<AttributeKey, AttributeValue>;

/// Digest over a canonical encoding of a subject state.
[[nodiscard]] FR_API Sha256Digest DigestOfState(const SubjectState& state) noexcept;

/// Maximum number of subjects accepted in one document by one call.
struct FR_API SubjectLimit {
  std::size_t value{0};
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_VALUE_HPP
