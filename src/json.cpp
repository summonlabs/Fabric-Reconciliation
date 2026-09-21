// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace summon {
namespace fabric_reconciliation {
namespace json {
namespace {

const std::string kEmptyText;
const std::vector<Value> kEmptyItems;
const std::vector<std::pair<std::string, Value>> kEmptyMembers;

constexpr int kDumpDepthLimit = 128;

Status Reject(const char* reason) {
  return Status(StatusCode::Rejected, ReasonCode::None, std::string("json: ") + reason);
}

bool IsWhitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

class Parser {
 public:
  Parser(std::string_view text, const ParseLimits& limits) : text_(text), limits_(limits) {}

  Status Run(Value& out) {
    if (text_.empty()) {
      return Reject("empty input");
    }
    if (text_.size() > limits_.max_bytes) {
      return Reject("input exceeds max_bytes");
    }
    Status status = ParseValue(0, out);
    if (!status.ok()) {
      return status;
    }
    SkipWhitespace();
    if (position_ != text_.size()) {
      return Reject("trailing content after the top-level value");
    }
    return Status::Ok();
  }

 private:
  void SkipWhitespace() noexcept {
    while (position_ < text_.size() && IsWhitespace(text_[position_])) {
      ++position_;
    }
  }

  bool AccountItem() {
    if (items_ >= limits_.max_items) {
      return false;
    }
    ++items_;
    return true;
  }

  Status ParseValue(std::size_t depth, Value& out) {
    if (depth > limits_.max_depth) {
      return Reject("depth exceeds max_depth");
    }
    if (!AccountItem()) {
      return Reject("document exceeds max_items");
    }
    SkipWhitespace();
    if (position_ >= text_.size()) {
      return Reject("unexpected end of input");
    }
    const char character = text_[position_];
    switch (character) {
      case '{':
        return ParseObject(depth, out);
      case '[':
        return ParseArray(depth, out);
      case '"': {
        std::string text;
        Status status = ParseString(text);
        if (!status.ok()) {
          return status;
        }
        out = Value::Text(std::move(text));
        return Status::Ok();
      }
      case 't':
        return ParseLiteral("true", Value::Boolean(true), out);
      case 'f':
        return ParseLiteral("false", Value::Boolean(false), out);
      case 'n':
        return ParseLiteral("null", Value::Null(), out);
      default:
        if (character == '-' || (character >= '0' && character <= '9')) {
          return ParseNumber(out);
        }
        return Reject("unexpected character where a value was expected");
    }
  }

  Status ParseLiteral(const char* literal, Value value, Value& out) {
    const std::size_t length = std::char_traits<char>::length(literal);
    if (text_.size() - position_ < length || text_.compare(position_, length, literal) != 0) {
      return Reject("invalid literal");
    }
    position_ += length;
    out = std::move(value);
    return Status::Ok();
  }

  Status ParseObject(std::size_t depth, Value& out) {
    ++position_;  // consume '{'
    Value object = Value::Object();
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      out = std::move(object);
      return Status::Ok();
    }
    for (;;) {
      SkipWhitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return Reject("object key must be a string");
      }
      std::string key;
      Status status = ParseString(key);
      if (!status.ok()) {
        return status;
      }
      SkipWhitespace();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return Reject("object member is missing its colon");
      }
      ++position_;
      Value value;
      status = ParseValue(depth + 1, value);
      if (!status.ok()) {
        return status;
      }
      if (object.size() >= limits_.max_object_members) {
        return Reject("object exceeds max_object_members");
      }
      if (object.Contains(key)) {
        return Reject("duplicate object key");
      }
      object.Set(std::move(key), std::move(value));
      SkipWhitespace();
      if (position_ >= text_.size()) {
        return Reject("unterminated object");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        out = std::move(object);
        return Status::Ok();
      }
      return Reject("expected a comma or a closing brace");
    }
  }

  Status ParseArray(std::size_t depth, Value& out) {
    ++position_;  // consume '['
    Value array = Value::Array();
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      out = std::move(array);
      return Status::Ok();
    }
    for (;;) {
      Value value;
      Status status = ParseValue(depth + 1, value);
      if (!status.ok()) {
        return status;
      }
      if (array.size() >= limits_.max_array_items) {
        return Reject("array exceeds max_array_items");
      }
      array.Push(std::move(value));
      SkipWhitespace();
      if (position_ >= text_.size()) {
        return Reject("unterminated array");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        out = std::move(array);
        return Status::Ok();
      }
      return Reject("expected a comma or a closing bracket");
    }
  }

  Status AppendCodePoint(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7fu) {
      out.push_back(static_cast<char>(code_point));
      return Status::Ok();
    }
    if (code_point <= 0x7ffu) {
      out.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
      return Status::Ok();
    }
    if (code_point <= 0xffffu) {
      out.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
      return Status::Ok();
    }
    if (code_point <= 0x10ffffu) {
      out.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
      return Status::Ok();
    }
    return Reject("code point out of range");
  }

  Status ParseHex4(std::uint32_t& out) {
    if (text_.size() - position_ < 4) {
      return Reject("truncated unicode escape");
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const char character = text_[position_ + static_cast<std::size_t>(index)];
      std::uint32_t nibble = 0;
      if (character >= '0' && character <= '9') {
        nibble = static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        nibble = static_cast<std::uint32_t>(character - 'a') + 10u;
      } else if (character >= 'A' && character <= 'F') {
        nibble = static_cast<std::uint32_t>(character - 'A') + 10u;
      } else {
        return Reject("invalid hex digit in unicode escape");
      }
      value = (value << 4) | nibble;
    }
    position_ += 4;
    out = value;
    return Status::Ok();
  }

  Status ParseString(std::string& out) {
    ++position_;  // consume opening quote
    std::string result;
    for (;;) {
      if (position_ >= text_.size()) {
        return Reject("unterminated string");
      }
      const unsigned char raw = static_cast<unsigned char>(text_[position_]);
      if (raw < 0x20u) {
        return Reject("raw control character inside a string");
      }
      if (raw == static_cast<unsigned char>('"')) {
        ++position_;
        out = std::move(result);
        return Status::Ok();
      }
      if (raw == static_cast<unsigned char>('\\')) {
        ++position_;
        if (position_ >= text_.size()) {
          return Reject("unterminated escape sequence");
        }
        const char escape = text_[position_];
        ++position_;
        switch (escape) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            std::uint32_t first = 0;
            Status status = ParseHex4(first);
            if (!status.ok()) {
              return status;
            }
            std::uint32_t code_point = first;
            if (first >= 0xd800u && first <= 0xdbffu) {
              if (text_.size() - position_ < 2 || text_[position_] != '\\' ||
                  text_[position_ + 1] != 'u') {
                return Reject("high surrogate without a low surrogate");
              }
              position_ += 2;
              std::uint32_t second = 0;
              status = ParseHex4(second);
              if (!status.ok()) {
                return status;
              }
              if (second < 0xdc00u || second > 0xdfffu) {
                return Reject("high surrogate followed by a non low surrogate");
              }
              code_point = 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
            } else if (first >= 0xdc00u && first <= 0xdfffu) {
              return Reject("lone low surrogate");
            }
            status = AppendCodePoint(result, code_point);
            if (!status.ok()) {
              return status;
            }
            break;
          }
          default:
            return Reject("invalid escape character");
        }
        if (result.size() > limits_.max_string_bytes) {
          return Reject("string exceeds max_string_bytes");
        }
        continue;
      }
      result.push_back(static_cast<char>(raw));
      ++position_;
      if (result.size() > limits_.max_string_bytes) {
        return Reject("string exceeds max_string_bytes");
      }
    }
  }

  Status ParseNumber(Value& out) {
    const std::size_t start = position_;
    const bool negative = (text_[position_] == '-');
    if (negative) {
      ++position_;
      if (position_ >= text_.size()) {
        return Reject("truncated number");
      }
    }
    if (text_[position_] == '0') {
      ++position_;
      if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        return Reject("number has a leading zero");
      }
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    } else {
      return Reject("number has no integer part");
    }

    bool is_real = false;
    if (position_ < text_.size() && text_[position_] == '.') {
      is_real = true;
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Reject("fraction has no digits");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      is_real = true;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') {
        return Reject("exponent has no digits");
      }
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
    }

    const std::string literal(text_.substr(start, position_ - start));
    if (is_real) {
      errno = 0;
      const double value = std::strtod(literal.c_str(), nullptr);
      if (errno == ERANGE && (value == 0.0 || !std::isfinite(value))) {
        return Reject("real number is out of range");
      }
      if (!std::isfinite(value)) {
        return Reject("real number is not finite");
      }
      out = Value::Real(value);
      return Status::Ok();
    }
    errno = 0;
    if (negative) {
      const long long value = std::strtoll(literal.c_str(), nullptr, 10);
      if (errno == ERANGE) {
        return Reject("integer literal is out of range");
      }
      out = Value::Integer(static_cast<std::int64_t>(value));
      return Status::Ok();
    }
    const unsigned long long value = std::strtoull(literal.c_str(), nullptr, 10);
    if (errno == ERANGE) {
      return Reject("integer literal is out of range");
    }
    if (value <= static_cast<unsigned long long>((std::numeric_limits<std::int64_t>::max)())) {
      out = Value::Integer(static_cast<std::int64_t>(value));
    } else {
      out = Value::Unsigned(static_cast<std::uint64_t>(value));
    }
    return Status::Ok();
  }

  std::string_view text_;
  ParseLimits limits_{};
  std::size_t position_{0};
  std::size_t items_{0};
};

void AppendEscaped(std::string& out, const std::string& text) {
  out.push_back('"');
  for (const char character : text) {
    const unsigned char raw = static_cast<unsigned char>(character);
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (raw < 0x20u) {
          static const char kDigits[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kDigits[(raw >> 4) & 0x0fu]);
          out.push_back(kDigits[raw & 0x0fu]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
}

void AppendInteger(std::string& out, std::int64_t value) {
  out += std::to_string(value);
}

void AppendUnsigned(std::string& out, std::uint64_t value) {
  out += std::to_string(value);
}

void AppendReal(std::string& out, double value) {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  out += buffer;
}

void DumpValue(const Value& value, int indent, int depth, bool sort_keys, std::string& out);

void AppendIndent(std::string& out, int indent, int depth) {
  if (indent <= 0) {
    return;
  }
  out.push_back('\n');
  out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

void DumpValue(const Value& value, int indent, int depth, bool sort_keys, std::string& out) {
  if (depth > kDumpDepthLimit) {
    out += "null";
    return;
  }
  switch (value.kind()) {
    case Kind::Null:
      out += "null";
      return;
    case Kind::Boolean:
      out += value.AsBool(false) ? "true" : "false";
      return;
    case Kind::Integer:
      AppendInteger(out, value.AsInteger(0));
      return;
    case Kind::Unsigned:
      AppendUnsigned(out, value.AsUnsigned(0));
      return;
    case Kind::Real:
      AppendReal(out, value.AsReal(0.0));
      return;
    case Kind::Text:
      AppendEscaped(out, value.AsText());
      return;
    case Kind::Array: {
      const std::vector<Value>& items = value.items();
      if (items.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        AppendIndent(out, indent, depth + 1);
        DumpValue(items[index], indent, depth + 1, sort_keys, out);
      }
      AppendIndent(out, indent, depth);
      out.push_back(']');
      return;
    }
    case Kind::Object: {
      const auto& members = value.members();
      if (members.empty()) {
        out += "{}";
        return;
      }
      std::vector<std::size_t> order;
      order.reserve(members.size());
      for (std::size_t index = 0; index < members.size(); ++index) {
        order.push_back(index);
      }
      if (sort_keys) {
        std::stable_sort(order.begin(), order.end(),
                         [&members](std::size_t lhs, std::size_t rhs) {
                           return members[lhs].first < members[rhs].first;
                         });
      }
      out.push_back('{');
      for (std::size_t position = 0; position < order.size(); ++position) {
        if (position != 0) {
          out.push_back(',');
        }
        AppendIndent(out, indent, depth + 1);
        AppendEscaped(out, members[order[position]].first);
        out.push_back(':');
        if (indent > 0) {
          out.push_back(' ');
        }
        DumpValue(members[order[position]].second, indent, depth + 1, sort_keys, out);
      }
      AppendIndent(out, indent, depth);
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

Value Value::Null() { return Value(); }

Value Value::Boolean(bool input) {
  Value value;
  value.kind_ = Kind::Boolean;
  value.bool_ = input;
  return value;
}

Value Value::Integer(std::int64_t input) {
  Value value;
  value.kind_ = Kind::Integer;
  value.int_ = input;
  return value;
}

Value Value::Unsigned(std::uint64_t input) {
  Value value;
  value.kind_ = Kind::Unsigned;
  value.uint_ = input;
  return value;
}

Value Value::Real(double input) {
  Value value;
  value.kind_ = Kind::Real;
  value.real_ = input;
  return value;
}

Value Value::Text(std::string input) {
  Value value;
  value.kind_ = Kind::Text;
  value.text_ = std::move(input);
  return value;
}

Value Value::Array() {
  Value value;
  value.kind_ = Kind::Array;
  return value;
}

Value Value::Object() {
  Value value;
  value.kind_ = Kind::Object;
  return value;
}

bool Value::AsBool(bool fallback) const noexcept {
  if (kind_ == Kind::Boolean) {
    return bool_;
  }
  return fallback;
}

std::int64_t Value::AsInteger(std::int64_t fallback) const noexcept {
  if (kind_ == Kind::Integer) {
    return int_;
  }
  if (kind_ == Kind::Unsigned) {
    if (uint_ > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
      return fallback;
    }
    return static_cast<std::int64_t>(uint_);
  }
  if (kind_ == Kind::Real) {
    if (!std::isfinite(real_)) {
      return fallback;
    }
    if (real_ < -9223372036854775808.0 || real_ > 9223372036854775807.0) {
      return fallback;
    }
    return static_cast<std::int64_t>(real_);
  }
  return fallback;
}

std::uint64_t Value::AsUnsigned(std::uint64_t fallback) const noexcept {
  if (kind_ == Kind::Unsigned) {
    return uint_;
  }
  if (kind_ == Kind::Integer) {
    if (int_ < 0) {
      return fallback;
    }
    return static_cast<std::uint64_t>(int_);
  }
  if (kind_ == Kind::Real) {
    if (!std::isfinite(real_) || real_ < 0.0 || real_ > 18446744073709551615.0) {
      return fallback;
    }
    return static_cast<std::uint64_t>(real_);
  }
  return fallback;
}

double Value::AsReal(double fallback) const noexcept {
  if (kind_ == Kind::Real) {
    return real_;
  }
  if (kind_ == Kind::Integer) {
    return static_cast<double>(int_);
  }
  if (kind_ == Kind::Unsigned) {
    return static_cast<double>(uint_);
  }
  return fallback;
}

const std::string& Value::AsText() const noexcept {
  if (kind_ == Kind::Text) {
    return text_;
  }
  return kEmptyText;
}

std::size_t Value::size() const noexcept {
  if (kind_ == Kind::Array) {
    return items_.size();
  }
  if (kind_ == Kind::Object) {
    return members_.size();
  }
  return 0;
}

const std::vector<Value>& Value::items() const noexcept {
  if (kind_ == Kind::Array) {
    return items_;
  }
  return kEmptyItems;
}

const std::vector<std::pair<std::string, Value>>& Value::members() const noexcept {
  if (kind_ == Kind::Object) {
    return members_;
  }
  return kEmptyMembers;
}

const Value* Value::Find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  for (const auto& member : members_) {
    if (member.first.size() == key.size() &&
        std::string_view(member.first).compare(key) == 0) {
      return &member.second;
    }
  }
  return nullptr;
}

bool Value::Contains(std::string_view key) const noexcept { return Find(key) != nullptr; }

bool Value::Set(std::string key, Value value) {
  if (kind_ != Kind::Object) {
    return false;
  }
  for (auto& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return true;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
  return true;
}

bool Value::Push(Value value) {
  if (kind_ != Kind::Array) {
    return false;
  }
  items_.push_back(std::move(value));
  return true;
}

std::string Value::Dump(int indent) const {
  std::string result;
  DumpValue(*this, indent, 0, false, result);
  return result;
}

std::string Value::DumpCanonical(int indent) const {
  std::string result;
  DumpValue(*this, indent, 0, true, result);
  return result;
}

Status Parse(std::string_view text, const ParseLimits& limits, Value& out) {
  Parser parser(text, limits);
  Value value;
  const Status status = parser.Run(value);
  if (!status.ok()) {
    return status;
  }
  out = std::move(value);
  return Status::Ok();
}

}  // namespace json
}  // namespace fabric_reconciliation
}  // namespace summon
