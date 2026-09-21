// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal strict JSON reader and writer.
//
// This module exists so that the command line tools, the fixtures and the
// protocol payloads can carry human-readable documents without pulling in a
// third-party dependency. It parses untrusted input and is therefore total:
// every malformed document produces a Rejected status rather than a crash, an
// unbounded allocation or a partial value.
//
// Deliberate strictness beyond RFC 8259:
//   * duplicate object keys are refused;
//   * trailing content after the top-level value is refused;
//   * integer literals that do not fit the target integer range are refused
//     instead of being rounded;
//   * depth, element count and string length are bounded by ParseLimits and the
//     bounds are checked before memory is reserved.
// Bytes >= 0x80 inside strings are passed through without UTF-8 validation;
// the runtime compares them byte for byte and never interprets them.

#ifndef SUMMON_FABRIC_RECONCILIATION_JSON_HPP
#define SUMMON_FABRIC_RECONCILIATION_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace json {

enum class Kind : std::uint8_t {
  Null = 0,
  Boolean = 1,
  Integer = 2,
  Unsigned = 3,
  Real = 4,
  Text = 5,
  Array = 6,
  Object = 7,
};

class FR_API Value {
 public:
  Value() = default;

  [[nodiscard]] static Value Null();
  [[nodiscard]] static Value Boolean(bool value);
  [[nodiscard]] static Value Integer(std::int64_t value);
  [[nodiscard]] static Value Unsigned(std::uint64_t value);
  [[nodiscard]] static Value Real(double value);
  [[nodiscard]] static Value Text(std::string value);
  [[nodiscard]] static Value Array();
  [[nodiscard]] static Value Object();

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool IsNull() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool IsObject() const noexcept { return kind_ == Kind::Object; }
  [[nodiscard]] bool IsArray() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool IsText() const noexcept { return kind_ == Kind::Text; }
  [[nodiscard]] bool IsNumber() const noexcept {
    return kind_ == Kind::Integer || kind_ == Kind::Unsigned || kind_ == Kind::Real;
  }

  [[nodiscard]] bool AsBool(bool fallback) const noexcept;
  [[nodiscard]] std::int64_t AsInteger(std::int64_t fallback) const noexcept;
  [[nodiscard]] std::uint64_t AsUnsigned(std::uint64_t fallback) const noexcept;
  [[nodiscard]] double AsReal(double fallback) const noexcept;
  [[nodiscard]] const std::string& AsText() const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const std::vector<Value>& items() const noexcept;
  [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const noexcept;

  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
  [[nodiscard]] bool Contains(std::string_view key) const noexcept;

  bool Set(std::string key, Value value);
  bool Push(Value value);

  [[nodiscard]] std::string Dump(int indent = 0) const;
  [[nodiscard]] std::string DumpCanonical(int indent = -1) const;

 private:
  Kind kind_{Kind::Null};
  bool bool_{false};
  std::int64_t int_{0};
  std::uint64_t uint_{0};
  double real_{0.0};
  std::string text_;
  std::vector<Value> items_;
  std::vector<std::pair<std::string, Value>> members_;
};

struct FR_API ParseLimits {
  std::size_t max_bytes = 16u * 1024u * 1024u;
  std::size_t max_depth = 64;
  std::size_t max_items = 4000000;
  std::size_t max_string_bytes = 1024u * 1024u;
  std::size_t max_object_members = 1000000;
  std::size_t max_array_items = 4000000;
};

/// Strict, total parse. On failure @p out is left unmodified.
[[nodiscard]] FR_API Status Parse(std::string_view text, const ParseLimits& limits, Value& out);

}  // namespace json
}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_JSON_HPP
