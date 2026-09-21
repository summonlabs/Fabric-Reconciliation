// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/value.hpp"

namespace summon {
namespace fabric_reconciliation {

const char* ToText(AttributeKind value) noexcept {
  switch (value) {
    case AttributeKind::Invalid: return "INVALID";
    case AttributeKind::Absent: return "ABSENT";
    case AttributeKind::Boolean: return "BOOLEAN";
    case AttributeKind::Integer: return "INTEGER";
    case AttributeKind::Unsigned: return "UNSIGNED";
    case AttributeKind::Text: return "TEXT";
    case AttributeKind::Token: return "TOKEN";
  }
  return "INVALID";
}

bool TryParseAttributeKind(const char* text, AttributeKind& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::string_view view(text);
  if (view == "ABSENT") { out = AttributeKind::Absent; return true; }
  if (view == "BOOLEAN") { out = AttributeKind::Boolean; return true; }
  if (view == "INTEGER") { out = AttributeKind::Integer; return true; }
  if (view == "UNSIGNED") { out = AttributeKind::Unsigned; return true; }
  if (view == "TEXT") { out = AttributeKind::Text; return true; }
  if (view == "TOKEN") { out = AttributeKind::Token; return true; }
  return false;
}

AttributeValue AttributeValue::Absent() {
  AttributeValue value;
  value.kind_ = AttributeKind::Absent;
  return value;
}

AttributeValue AttributeValue::Boolean(bool input) {
  AttributeValue value;
  value.kind_ = AttributeKind::Boolean;
  value.bool_ = input;
  return value;
}

AttributeValue AttributeValue::Integer(std::int64_t input) {
  AttributeValue value;
  value.kind_ = AttributeKind::Integer;
  value.int_ = input;
  return value;
}

AttributeValue AttributeValue::Unsigned(std::uint64_t input) {
  AttributeValue value;
  value.kind_ = AttributeKind::Unsigned;
  value.uint_ = input;
  return value;
}

AttributeValue AttributeValue::Text(std::string input) {
  AttributeValue value;
  value.kind_ = AttributeKind::Text;
  value.text_ = std::move(input);
  return value;
}

AttributeValue AttributeValue::Token(std::string input) {
  AttributeValue value;
  value.kind_ = AttributeKind::Token;
  value.text_ = std::move(input);
  return value;
}

bool AttributeValue::IsValid() const noexcept {
  switch (kind_) {
    case AttributeKind::Invalid:
      return false;
    case AttributeKind::Absent:
    case AttributeKind::Boolean:
    case AttributeKind::Integer:
    case AttributeKind::Unsigned:
      return text_.empty();
    case AttributeKind::Text:
    case AttributeKind::Token:
      return text_.size() <= kMaxAttributeTextLength;
  }
  return false;
}

void AttributeValue::Encode(CanonicalWriter& writer) const noexcept {
  writer.PutU8(static_cast<std::uint8_t>(kind_));
  switch (kind_) {
    case AttributeKind::Boolean:
      writer.PutBool(bool_);
      break;
    case AttributeKind::Integer:
      writer.PutI64(int_);
      break;
    case AttributeKind::Unsigned:
      writer.PutU64(uint_);
      break;
    case AttributeKind::Text:
    case AttributeKind::Token:
      writer.PutBytes(text_);
      break;
    case AttributeKind::Absent:
    case AttributeKind::Invalid:
      break;
  }
}

bool AttributeValue::Decode(CanonicalReader& reader, AttributeValue& out) {
  std::uint8_t raw_kind = 0;
  if (!reader.ReadU8(raw_kind)) {
    return false;
  }
  AttributeValue value;
  switch (static_cast<AttributeKind>(raw_kind)) {
    case AttributeKind::Absent:
      value = AttributeValue::Absent();
      break;
    case AttributeKind::Boolean: {
      bool flag = false;
      if (!reader.ReadBool(flag)) {
        return false;
      }
      value = AttributeValue::Boolean(flag);
      break;
    }
    case AttributeKind::Integer: {
      std::int64_t number = 0;
      if (!reader.ReadI64(number)) {
        return false;
      }
      value = AttributeValue::Integer(number);
      break;
    }
    case AttributeKind::Unsigned: {
      std::uint64_t number = 0;
      if (!reader.ReadU64(number)) {
        return false;
      }
      value = AttributeValue::Unsigned(number);
      break;
    }
    case AttributeKind::Text:
    case AttributeKind::Token: {
      std::string text;
      if (!reader.ReadBytes(text)) {
        return false;
      }
      if (text.size() > kMaxAttributeTextLength) {
        return false;
      }
      value = (static_cast<AttributeKind>(raw_kind) == AttributeKind::Text)
                  ? AttributeValue::Text(std::move(text))
                  : AttributeValue::Token(std::move(text));
      break;
    }
    case AttributeKind::Invalid:
    default:
      return false;
  }
  out = std::move(value);
  return true;
}

std::string AttributeValue::Render() const {
  switch (kind_) {
    case AttributeKind::Absent:
      return "<absent>";
    case AttributeKind::Boolean:
      return bool_ ? "true" : "false";
    case AttributeKind::Integer:
      return std::to_string(int_);
    case AttributeKind::Unsigned:
      return std::to_string(uint_);
    case AttributeKind::Text:
      return "text:" + text_;
    case AttributeKind::Token:
      return text_;
    case AttributeKind::Invalid:
    default:
      return "<invalid>";
  }
}

Sha256Digest DigestOfState(const SubjectState& state) noexcept {
  CanonicalWriter writer;
  writer.PutU32(static_cast<std::uint32_t>(state.size() & 0xffffffffu));
  for (const auto& entry : state) {
    writer.PutBytes(entry.first.str());
    entry.second.Encode(writer);
  }
  return writer.Digest();
}

}  // namespace fabric_reconciliation
}  // namespace summon
