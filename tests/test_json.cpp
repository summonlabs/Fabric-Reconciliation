// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strictness and totality proofs for the JSON reader.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

#include "summon/fabric_reconciliation/json.hpp"

namespace json = summon::fabric_reconciliation::json;
using JsonStatus = summon::fabric_reconciliation::Status;

namespace {

JsonStatus Parse(const std::string& text, json::Value& out) {
  return json::Parse(text, json::ParseLimits{}, out);
}

bool Accepts(const std::string& text) {
  json::Value value;
  return Parse(text, value).ok();
}

bool Rejects(const std::string& text) {
  json::Value value;
  return !Parse(text, value).ok();
}

}  // namespace

FR_TEST(json, parses_every_value_kind) {
  json::Value value;
  FR_CHECK(Parse("null", value).ok());
  FR_CHECK(value.IsNull());
  FR_CHECK(Parse("true", value).ok());
  FR_CHECK(value.AsBool(false));
  FR_CHECK(Parse("-17", value).ok());
  FR_CHECK_EQ(value.AsInteger(0), static_cast<std::int64_t>(-17));
  FR_CHECK(Parse("18446744073709551615", value).ok());
  FR_CHECK_EQ(value.AsUnsigned(0), 18446744073709551615ull);
  FR_CHECK(Parse("1.5e3", value).ok());
  FR_CHECK_EQ(value.AsReal(0.0), 1500.0);
  FR_CHECK(Parse("\"text\"", value).ok());
  FR_CHECK_EQ(value.AsText(), std::string("text"));
  FR_CHECK(Parse("[]", value).ok());
  FR_CHECK(value.IsArray());
  FR_CHECK(Parse("{}", value).ok());
  FR_CHECK(value.IsObject());
}

FR_TEST(json, round_trips_nested_documents) {
  const std::string text =
      "{\"b\":1,\"a\":[true,null,{\"c\":\"x\"}],\"d\":{\"e\":-2.5}}";
  json::Value value;
  FR_CHECK(Parse(text, value).ok());
  const std::string dumped = value.Dump(0);
  json::Value reparsed;
  FR_CHECK(Parse(dumped, reparsed).ok());
  FR_CHECK_EQ(dumped, reparsed.Dump(0));
  FR_CHECK_EQ(value.DumpCanonical(), reparsed.DumpCanonical());
}

FR_TEST(json, canonical_dump_sorts_keys_and_plain_dump_does_not) {
  json::Value value;
  FR_CHECK(Parse("{\"b\":1,\"a\":2}", value).ok());
  FR_CHECK_EQ(value.Dump(0), std::string("{\"b\":1,\"a\":2}"));
  FR_CHECK_EQ(value.DumpCanonical(), std::string("{\"a\":2,\"b\":1}"));
}

FR_TEST(json, pretty_dump_is_stable) {
  json::Value value;
  FR_CHECK(Parse("{\"a\":[1,2]}", value).ok());
  const std::string pretty = value.Dump(2);
  FR_CHECK_EQ(pretty, std::string("{\n  \"a\": [\n    1,\n    2\n  ]\n}"));
}

FR_TEST(json, unicode_escapes_and_surrogate_pairs) {
  json::Value value;
  FR_CHECK(Parse("\"\\u0041\\u00e9\\u20ac\"", value).ok());
  FR_CHECK_EQ(value.AsText(), std::string("A\xc3\xa9\xe2\x82\xac"));
  FR_CHECK(Parse("\"\\ud83d\\ude00\"", value).ok());
  FR_CHECK_EQ(value.AsText(), std::string("\xf0\x9f\x98\x80"));
  FR_CHECK(Accepts("\"\\u0000\""));
}

FR_TEST(json, rejects_lone_surrogates_and_bad_escapes) {
  FR_CHECK(Rejects("\"\\ud83d\""));
  FR_CHECK(Rejects("\"\\ude00\""));
  FR_CHECK(Rejects("\"\\ud83dx\""));
  FR_CHECK(Rejects("\"\\ud83d\\u0041\""));
  FR_CHECK(Rejects("\"\\u12\""));
  FR_CHECK(Rejects("\"\\u12g4\""));
  FR_CHECK(Rejects("\"\\q\""));
  FR_CHECK(Rejects("\"\\\""));
}

FR_TEST(json, rejects_unterminated_constructs) {
  FR_CHECK(Rejects("\"abc"));
  FR_CHECK(Rejects("{\"a\":1"));
  FR_CHECK(Rejects("[1,2"));
  FR_CHECK(Rejects("{"));
  FR_CHECK(Rejects("["));
  FR_CHECK(Rejects(""));
  FR_CHECK(Rejects("   "));
  FR_CHECK(Rejects("{,}"));
  FR_CHECK(Rejects("[1,]"));
  FR_CHECK(Rejects("{\"a\":}"));
  FR_CHECK(Rejects("{\"a\" 1}"));
  FR_CHECK(Rejects("{a:1}"));
}

FR_TEST(json, rejects_raw_control_characters_in_strings) {
  FR_CHECK(Rejects("\"a\x01b\""));
  FR_CHECK(Rejects("\"a\nb\""));
  FR_CHECK(Rejects(std::string("\"a\0b\"", 5)));
}

FR_TEST(json, rejects_duplicate_keys_including_escaped_equivalents) {
  FR_CHECK(Rejects("{\"a\":1,\"a\":2}"));
  FR_CHECK(Rejects("{\"a\":1,\"\\u0061\":2}"));
}

FR_TEST(json, rejects_trailing_content) {
  FR_CHECK(Rejects("1 2"));
  FR_CHECK(Rejects("{} {}"));
  FR_CHECK(Rejects("null,"));
  FR_CHECK(Rejects("[1]x"));
}

FR_TEST(json, rejects_malformed_numbers) {
  FR_CHECK(Rejects("+1"));
  FR_CHECK(Rejects(".5"));
  FR_CHECK(Rejects("5."));
  FR_CHECK(Rejects("01"));
  FR_CHECK(Rejects("-01"));
  FR_CHECK(Rejects("1e"));
  FR_CHECK(Rejects("1e+"));
  FR_CHECK(Rejects("--1"));
  FR_CHECK(Rejects("-"));
  FR_CHECK(Rejects("Infinity"));
  FR_CHECK(Rejects("NaN"));
  FR_CHECK(Rejects("0x10"));
  FR_CHECK(Rejects("1.2.3"));
}

FR_TEST(json, rejects_numbers_out_of_range) {
  FR_CHECK(Rejects("18446744073709551616"));
  FR_CHECK(Rejects("-9223372036854775809"));
  FR_CHECK(Rejects("1e400"));
  FR_CHECK(Rejects("-1e400"));
  FR_CHECK(Accepts("-9223372036854775808"));
  FR_CHECK(Accepts("9223372036854775807"));
}

FR_TEST(json, accepts_valid_whitespace_forms) {
  FR_CHECK(Accepts(" \t\r\n { \"a\" : [ 1 , 2 ] }\n"));
}

FR_TEST(json, depth_limit_is_enforced) {
  std::string nested;
  for (int index = 0; index < 5000; ++index) {
    nested.push_back('[');
  }
  for (int index = 0; index < 5000; ++index) {
    nested.push_back(']');
  }
  json::Value value;
  const JsonStatus status = Parse(nested, value);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == summon::fabric_reconciliation::StatusCode::Rejected);
}

FR_TEST(json, item_limit_is_enforced_before_materialisation) {
  std::string document = "[";
  for (int index = 0; index < 200000; ++index) {
    if (index != 0) {
      document.push_back(',');
    }
    document.push_back('0');
  }
  document.push_back(']');

  json::ParseLimits small;
  small.max_items = 100;
  json::Value value;
  FR_CHECK(!json::Parse(document, small, value).ok());

  json::ParseLimits large;
  large.max_items = 400000;
  FR_CHECK(json::Parse(document, large, value).ok());
  FR_CHECK_EQ(value.size(), std::size_t(200000));
}

FR_TEST(json, array_and_object_limits_are_enforced) {
  std::string array = "[";
  for (int index = 0; index < 50; ++index) {
    if (index != 0) {
      array.push_back(',');
    }
    array.push_back('1');
  }
  array.push_back(']');
  json::ParseLimits array_limits;
  array_limits.max_array_items = 10;
  json::Value value;
  FR_CHECK(!json::Parse(array, array_limits, value).ok());

  std::string object = "{";
  for (int index = 0; index < 50; ++index) {
    if (index != 0) {
      object.push_back(',');
    }
    object += "\"k" + std::to_string(index) + "\":1";
  }
  object.push_back('}');
  json::ParseLimits object_limits;
  object_limits.max_object_members = 10;
  FR_CHECK(!json::Parse(object, object_limits, value).ok());
}

FR_TEST(json, string_length_limit_is_enforced) {
  std::string document = "\"";
  document.append(5000, 'a');
  document += "\"";
  json::ParseLimits limits;
  limits.max_string_bytes = 100;
  json::Value value;
  FR_CHECK(!json::Parse(document, limits, value).ok());
}

FR_TEST(json, input_size_limit_is_enforced) {
  json::ParseLimits limits;
  limits.max_bytes = 4;
  json::Value value;
  FR_CHECK(!json::Parse("{\"a\":1}", limits, value).ok());
}

FR_TEST(json, failed_parse_leaves_the_output_untouched) {
  json::Value value = json::Value::Text("sentinel");
  FR_CHECK(!Parse("{", value).ok());
  FR_CHECK_EQ(value.AsText(), std::string("sentinel"));
}

namespace {

/// Builds a deterministic, well-formed JSON document of bounded shape.
std::string GenerateDocument(frtest::Rng& rng, int depth) {
  const std::uint64_t choice = rng.Below(depth > 3 ? 5 : 7);
  switch (choice) {
    case 0:
      return "null";
    case 1:
      return rng.Coin() ? "true" : "false";
    case 2:
      return std::to_string(static_cast<long long>(rng.Next() % 1000000) - 500000);
    case 3:
      return "1.5e" + std::to_string(rng.Below(20));
    case 4: {
      static const char* kTexts[] = {"", "a", "hello world", "quote\\\"inside",
                                     "tab\\there", "slash\\\\here", "line\\nbreak",
                                     "\xc3\xa9\xe2\x82\xac"};
      return std::string("\"") + kTexts[rng.Below(8)] + "\"";
    }
    case 5: {
      const std::size_t count = static_cast<std::size_t>(rng.Below(4));
      std::string result = "[";
      for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
          result += ",";
        }
        result += GenerateDocument(rng, depth + 1);
      }
      result += "]";
      return result;
    }
    default: {
      const std::size_t count = static_cast<std::size_t>(rng.Below(4));
      std::string result = "{";
      for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
          result += ",";
        }
        result += "\"k" + std::to_string(depth) + "_" + std::to_string(index) + "\":";
        result += GenerateDocument(rng, depth + 1);
      }
      result += "}";
      return result;
    }
  }
}

}  // namespace

FR_TEST(json, fuzz_never_crashes_or_hangs) {
  frtest::Rng rng(0x9e3779b97f4a7c15ull);
  const char* alphabet = "{}[]\":,0123456789.eE+-truefalsn\\u\"\n\t ";
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.Below(40));
    std::string text;
    for (std::size_t index = 0; index < length; ++index) {
      text.push_back(alphabet[rng.Below(30)]);
    }
    json::Value value;
    const JsonStatus status = Parse(text, value);
    if (status.ok()) {
      ++accepted;
      const std::string dumped = value.Dump(0);
      json::Value reparsed;
      FR_CHECK(Parse(dumped, reparsed).ok());
      FR_CHECK_EQ(dumped, reparsed.Dump(0));
    } else {
      // Random bytes are almost never valid JSON, so the property that matters
      // here is totality: every input either parses or is refused, and a
      // refusal is always a Rejected status rather than anything else.
      FR_CHECK(status.code() == summon::fabric_reconciliation::StatusCode::Rejected);
    }
  }
  // The corpus must exercise the accepting path at least occasionally,
  // otherwise the round-trip half of this property proves nothing.
  FR_CHECK(accepted > 5);
}

FR_TEST(json, generated_documents_round_trip_exactly) {
  frtest::Rng rng(0x1234567890abcdefull);
  std::size_t generated = 0;
  for (int iteration = 0; iteration < 3000; ++iteration) {
    const std::string document = GenerateDocument(rng, 0);
    json::Value value;
    const JsonStatus parsed = Parse(document, value);
    if (!parsed.ok()) {
      FR_CHECK(false);
      continue;
    }
    const std::string compact = value.Dump(0);
    json::Value reparsed;
    FR_CHECK(Parse(compact, reparsed).ok());
    FR_CHECK_EQ(compact, reparsed.Dump(0));
    FR_CHECK_EQ(value.DumpCanonical(), reparsed.DumpCanonical());
    // A pretty dump must reparse to the same canonical form.
    json::Value pretty;
    FR_CHECK(Parse(value.Dump(2), pretty).ok());
    FR_CHECK_EQ(pretty.DumpCanonical(), value.DumpCanonical());
    ++generated;
  }
  FR_CHECK_EQ(generated, std::size_t(3000));
}

FR_TEST(json, mutation_helpers_reject_wrong_kinds) {
  json::Value array = json::Value::Array();
  FR_CHECK(!array.Set("k", json::Value::Null()));
  FR_CHECK(array.Push(json::Value::Integer(1)));
  FR_CHECK_EQ(array.size(), std::size_t(1));
  json::Value object = json::Value::Object();
  FR_CHECK(!object.Push(json::Value::Null()));
  FR_CHECK(object.Set("k", json::Value::Integer(1)));
  FR_CHECK(object.Contains("k"));
  FR_CHECK(object.Set("k", json::Value::Integer(2)));
  FR_CHECK_EQ(object.size(), std::size_t(1));
  FR_CHECK_EQ(object.Find("k")->AsInteger(0), static_cast<std::int64_t>(2));
  FR_CHECK(object.Find("missing") == nullptr);
}

FR_TEST(json, deep_dump_does_not_overflow_the_stack) {
  json::Value nested = json::Value::Array();
  for (int index = 0; index < 400; ++index) {
    json::Value inner = json::Value::Array();
    inner.Push(std::move(nested));
    nested = std::move(inner);
  }
  const std::string dumped = nested.Dump(0);
  FR_CHECK(!dumped.empty());
  FR_CHECK(dumped.find("null") != std::string::npos);
}
