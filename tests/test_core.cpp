// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Unit proofs for identities, checked arithmetic, digests, canonical encoding,
// attribute values and the deterministic document surface.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

FR_TEST(identity, accepts_well_formed_text) {
  FR_CHECK(fr::IsValidIdentityText("fabric/rack-7"));
  FR_CHECK(fr::IsValidIdentityText("node.n1_port-2:queue3"));
  FR_CHECK(fr::IsValidIdentityText("a"));
  FR_CHECK(fr::IsValidIdentityText(std::string(fr::kMaxIdentityLength, 'a')));
}

FR_TEST(identity, rejects_malformed_text) {
  FR_CHECK(!fr::IsValidIdentityText(""));
  FR_CHECK(!fr::IsValidIdentityText(std::string(fr::kMaxIdentityLength + 1, 'a')));
  FR_CHECK(!fr::IsValidIdentityText("/leading"));
  FR_CHECK(!fr::IsValidIdentityText("trailing/"));
  FR_CHECK(!fr::IsValidIdentityText("double//separator"));
  FR_CHECK(!fr::IsValidIdentityText("back\\slash"));
  FR_CHECK(!fr::IsValidIdentityText("space here"));
  FR_CHECK(!fr::IsValidIdentityText(".."));
  FR_CHECK(!fr::IsValidIdentityText("../etc/passwd"));
  FR_CHECK(!fr::IsValidIdentityText("fabric/../rack"));
  FR_CHECK(!fr::IsValidIdentityText("fabric/./rack"));
  FR_CHECK(!fr::IsValidIdentityText(".hidden"));
  FR_CHECK(!fr::IsValidIdentityText(std::string_view("nul\0byte", 8)));
  FR_CHECK(!fr::IsValidIdentityText(std::string("control") + '\x01' + "byte"));
  FR_CHECK(!fr::IsValidIdentityText(std::string("del") + '\x7f' + "byte"));
}

FR_TEST(identity, try_parse_returns_nullopt_for_invalid) {
  FR_CHECK(!fr::ScopeId::TryParse("bad scope").has_value());
  FR_CHECK(fr::ScopeId::TryParse("good/scope").has_value());
}

FR_TEST(identity, distinct_tags_are_distinct_types) {
  const fr::ScopeId scope = RequireScope("fabric/rack-7");
  const fr::SubjectId subject = RequireSubject("fabric/rack-7");
  // The compiler must not allow a ScopeId to be compared with a SubjectId.
  // This test documents the intent; the real proof is that the following line
  // does not compile:  bool same = (scope == subject);
  FR_CHECK_EQ(scope.str(), subject.str());
  FR_CHECK(scope == *fr::ScopeId::TryParse(subject.str()));
}

FR_TEST(identity, boot_ids_are_unique_across_threads) {
  std::vector<fr::BootId> ids(64);
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    threads.emplace_back([&ids, index]() { ids[index] = fr::BootId::Generate(); });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  std::set<std::string> unique;
  for (const fr::BootId& id : ids) {
    unique.insert(id.ToHex());
  }
  FR_CHECK_EQ(unique.size(), ids.size());
}

FR_TEST(identity, store_id_zero_detection) {
  const fr::StoreId zero = fr::StoreId::FromBytes(std::array<std::uint8_t, 16>{});
  FR_CHECK(zero.IsZero());
  FR_CHECK(!fr::StoreId::Generate().IsZero());
}

FR_TEST(counter, next_refuses_to_wrap) {
  const fr::CoordinatorEpoch maximum((std::numeric_limits<std::uint64_t>::max)());
  fr::CoordinatorEpoch out;
  FR_CHECK(!maximum.TryNext(out));
  const fr::CoordinatorEpoch small(41);
  FR_CHECK(small.TryNext(out));
  FR_CHECK_EQ(out.value(), 42ull);
}

FR_TEST(arithmetic, checked_helpers_detect_wrap) {
  const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
  FR_CHECK(fr::AddWouldOverflow(maximum, 1));
  FR_CHECK(!fr::AddWouldOverflow(maximum - 1, 1));
  FR_CHECK(fr::MulWouldOverflow(maximum, 2));
  FR_CHECK(!fr::MulWouldOverflow(0, maximum));
  std::uint64_t aligned = 0;
  FR_CHECK(fr::AlignUpChecked(10, 8, aligned));
  FR_CHECK_EQ(aligned, 16ull);
  FR_CHECK(fr::AlignUpChecked(16, 8, aligned));
  FR_CHECK_EQ(aligned, 16ull);
  FR_CHECK(!fr::AlignUpChecked(maximum, 8, aligned));
  FR_CHECK(!fr::AlignUpChecked(1, 0, aligned));
}

FR_TEST(sha256, known_vectors) {
  FR_CHECK_EQ(fr::ToHex(fr::Sha256Of(std::string_view(""))),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FR_CHECK_EQ(fr::ToHex(fr::Sha256Of(std::string_view("abc"))),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FR_CHECK_EQ(
      fr::ToHex(fr::Sha256Of(std::string_view(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

FR_TEST(sha256, streaming_matches_single_shot) {
  std::string payload;
  for (int index = 0; index < 5000; ++index) {
    payload.push_back(static_cast<char>('a' + (index % 26)));
  }
  fr::Sha256 streamed;
  for (std::size_t offset = 0; offset < payload.size(); offset += 7) {
    streamed.Update(payload.data() + offset, std::min<std::size_t>(7, payload.size() - offset));
  }
  FR_CHECK(streamed.Final() == fr::Sha256Of(payload));
}

FR_TEST(sha256, hex_parsing_is_strict) {
  const fr::Sha256Digest digest = fr::Sha256Of(std::string_view("abc"));
  const std::string text = fr::ToHex(digest);
  fr::Sha256Digest parsed{};
  FR_CHECK(fr::TryParseHexDigest(text, parsed));
  FR_CHECK(parsed == digest);
  FR_CHECK(fr::TryParseHexDigest("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", parsed));
  FR_CHECK(parsed == digest);
  FR_CHECK(!fr::TryParseHexDigest(text.substr(0, 63), parsed));
  FR_CHECK(!fr::TryParseHexDigest(text + "0", parsed));
  FR_CHECK(!fr::TryParseHexDigest("", parsed));
  std::string non_hex = text;
  non_hex[5] = 'z';
  FR_CHECK(!fr::TryParseHexDigest(non_hex, parsed));
}

FR_TEST(canonical, integers_round_trip_at_every_boundary) {
  const std::uint64_t values[] = {0ull, 1ull, 255ull, 256ull, 65535ull, 4294967295ull,
                                  (std::numeric_limits<std::uint64_t>::max)()};
  for (const std::uint64_t value : values) {
    fr::CanonicalWriter writer;
    writer.PutU64(value);
    fr::CanonicalReader reader(writer.buffer());
    std::uint64_t decoded = 0;
    FR_CHECK(reader.ReadU64(decoded));
    FR_CHECK(reader.AtEnd());
    FR_CHECK_EQ(decoded, value);
  }
  fr::CanonicalWriter writer;
  writer.PutI64(-1);
  fr::CanonicalReader reader(writer.buffer());
  std::int64_t negative = 0;
  FR_CHECK(reader.ReadI64(negative));
  FR_CHECK_EQ(negative, static_cast<std::int64_t>(-1));
}

FR_TEST(canonical, reader_is_total_and_sticky) {
  const std::string buffer = "\x01\x02";
  fr::CanonicalReader reader(buffer);
  std::uint8_t first = 0;
  FR_CHECK(reader.ReadU8(first));
  FR_CHECK_EQ(first, static_cast<std::uint8_t>(1));
  std::uint64_t too_big = 0;
  FR_CHECK(!reader.ReadU64(too_big));
  FR_CHECK(!reader.ok());
  // Sticky: every read after a failure fails, and AtEnd() is false.
  std::uint8_t never = 0;
  FR_CHECK(!reader.ReadU8(never));
  FR_CHECK(!reader.AtEnd());
  std::string bytes;
  FR_CHECK(!reader.ReadBytes(bytes));
}

FR_TEST(canonical, length_prefix_beyond_buffer_fails_without_allocating) {
  fr::CanonicalWriter writer;
  writer.PutU32(0xffffffffu);
  fr::CanonicalReader reader(writer.buffer());
  std::string out;
  FR_CHECK(!reader.ReadBytes(out));
  FR_CHECK(out.empty());
}

FR_TEST(canonical, writer_refuses_past_the_hard_ceiling) {
  fr::CanonicalWriter writer(fr::kMaxCanonicalBytes);
  FR_CHECK(writer.ok());
  const std::string chunk(1u << 20, 'x');
  for (int index = 0; index < 65; ++index) {
    writer.PutBytes(chunk);
  }
  FR_CHECK(!writer.ok());
}

FR_TEST(value, kinds_are_never_coerced) {
  const fr::AttributeValue integer = fr::AttributeValue::Integer(5);
  const fr::AttributeValue unsigned_value = fr::AttributeValue::Unsigned(5);
  const fr::AttributeValue text = fr::AttributeValue::Text("5");
  FR_CHECK(!integer.ComparableWith(unsigned_value));
  FR_CHECK(!integer.ComparableWith(text));
  FR_CHECK(!(integer == unsigned_value));
  FR_CHECK(integer.ComparableWith(fr::AttributeValue::Integer(5)));
}

FR_TEST(value, canonical_round_trip) {
  const fr::AttributeValue values[] = {
      fr::AttributeValue::Absent(), fr::AttributeValue::Boolean(true),
      fr::AttributeValue::Integer(-42), fr::AttributeValue::Unsigned(42),
      fr::AttributeValue::Text("hello"), fr::AttributeValue::Token("up")};
  for (const fr::AttributeValue& value : values) {
    fr::CanonicalWriter writer;
    value.Encode(writer);
    fr::CanonicalReader reader(writer.buffer());
    fr::AttributeValue decoded;
    FR_CHECK(fr::AttributeValue::Decode(reader, decoded));
    FR_CHECK(reader.AtEnd());
    FR_CHECK(decoded == value);
  }
}

FR_TEST(value, decode_rejects_unknown_kind_and_trailing_bytes) {
  const std::string unknown_kind = std::string("\x7f", 1);
  fr::CanonicalReader reader(unknown_kind);
  fr::AttributeValue value;
  FR_CHECK(!fr::AttributeValue::Decode(reader, value));

  fr::CanonicalWriter writer;
  fr::AttributeValue::Boolean(false).Encode(writer);
  writer.PutU8(0);
  fr::CanonicalReader trailing(writer.buffer());
  FR_CHECK(fr::AttributeValue::Decode(trailing, value));
  FR_CHECK(!trailing.AtEnd());
}

FR_TEST(value, over_long_text_is_rejected_on_validation_and_decode) {
  const std::string huge(fr::kMaxAttributeTextLength + 1, 'z');
  const fr::AttributeValue value = fr::AttributeValue::Text(huge);
  FR_CHECK(!value.IsValid());
  fr::CanonicalWriter writer;
  writer.PutU8(static_cast<std::uint8_t>(fr::AttributeKind::Text));
  writer.PutBytes(huge);
  fr::CanonicalReader reader(writer.buffer());
  fr::AttributeValue decoded;
  FR_CHECK(!fr::AttributeValue::Decode(reader, decoded));
}

FR_TEST(value, state_digest_is_order_independent) {
  fr::SubjectState first;
  first.emplace(RequireAttribute("mtu"), fr::AttributeValue::Unsigned(9000));
  first.emplace(RequireAttribute("admin-up"), fr::AttributeValue::Boolean(true));
  fr::SubjectState second;
  second.emplace(RequireAttribute("admin-up"), fr::AttributeValue::Boolean(true));
  second.emplace(RequireAttribute("mtu"), fr::AttributeValue::Unsigned(9000));
  FR_CHECK(fr::DigestOfState(first) == fr::DigestOfState(second));
  second[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(1500);
  FR_CHECK(!(fr::DigestOfState(first) == fr::DigestOfState(second)));
}

FR_TEST(document, intent_digest_is_content_addressed) {
  const fr::IntentDocument first = MakeIntent("fabric/rack-7", 1, 4, fr::EvidenceClass::Synthetic);
  const fr::IntentDocument second = MakeIntent("fabric/rack-7", 1, 4, fr::EvidenceClass::Synthetic);
  FR_CHECK(first.digest == second.digest);
  fr::IntentDocument third = second;
  third.generation = fr::IntentGeneration(2);
  third.digest = fr::ComputeIntentDigest(third);
  FR_CHECK(!(third.digest == second.digest));
}

FR_TEST(document, canonical_round_trip_preserves_everything) {
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 3, 6, fr::EvidenceClass::Synthetic);
  const std::string encoded = fr::EncodeIntent(intent);
  FR_CHECK(!encoded.empty());
  fr::IntentDocument decoded;
  FR_CHECK(fr::DecodeIntentExact(encoded, decoded, fr::RuntimeLimits{}));
  FR_CHECK(decoded == intent);
}

FR_TEST(document, decoding_rejects_trailing_garbage_and_tampering) {
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 3, 2, fr::EvidenceClass::Synthetic);
  std::string encoded = fr::EncodeIntent(intent);
  fr::IntentDocument decoded;
  FR_CHECK(!fr::DecodeIntentExact(encoded + std::string(1, '\0'), decoded, fr::RuntimeLimits{}));

  // Flip a byte inside the subject map: the recomputed digest no longer matches.
  std::string tampered = encoded;
  tampered[tampered.size() / 2] = static_cast<char>(tampered[tampered.size() / 2] ^ 0x01);
  FR_CHECK(!fr::DecodeIntentExact(tampered, decoded, fr::RuntimeLimits{}));
}

FR_TEST(document, every_truncated_prefix_is_refused) {
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 3, 3, fr::EvidenceClass::Synthetic);
  const std::string encoded = fr::EncodeIntent(intent);
  fr::IntentDocument decoded;
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    FR_CHECK(!fr::DecodeIntentExact(encoded.substr(0, length), decoded, fr::RuntimeLimits{}));
  }
  FR_CHECK(fr::DecodeIntentExact(encoded, decoded, fr::RuntimeLimits{}));
}

FR_TEST(document, validation_refuses_oversized_documents) {
  fr::RuntimeLimits limits;
  limits.max_subjects_per_document = 4;
  const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 5, fr::EvidenceClass::Synthetic);
  const fr::Status status = fr::ValidateIntent(intent, limits);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::LimitExceeded);
}

FR_TEST(document, observation_digest_binds_the_receipt_stamp) {
  const fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 3, 5, false, fr::EvidenceClass::Synthetic);
  fr::ObservationDocument first;
  first.observation = submission.observation;
  first.scope = submission.scope;
  first.reporter = submission.reporter;
  first.generation = submission.generation;
  first.evidence = submission.evidence;
  first.complete = submission.complete;
  first.authoritative_absence = submission.authoritative_absence;
  first.received_epoch = fr::CoordinatorEpoch(1);
  first.received_unix_ms = 1000;
  first.subjects = submission.subjects;
  first.digest = fr::ComputeObservationDigest(first);

  fr::ObservationDocument second = first;
  second.received_unix_ms = 2000;
  second.digest = fr::ComputeObservationDigest(second);
  FR_CHECK(!(first.digest == second.digest));

  const std::string encoded = fr::EncodeObservation(first);
  fr::ObservationDocument decoded;
  FR_CHECK(fr::DecodeObservationExact(encoded, decoded, fr::RuntimeLimits{}));
  FR_CHECK(decoded == first);
}

FR_TEST(document, submission_round_trip_and_bounds) {
  const fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 2, 4, 3, true, fr::EvidenceClass::Synthetic);
  const std::string encoded = fr::EncodeSubmission(submission);
  fr::ObservationSubmission decoded;
  FR_CHECK(fr::DecodeSubmissionExact(encoded, decoded, fr::RuntimeLimits{}));
  FR_CHECK(decoded == submission);

  fr::RuntimeLimits limits;
  limits.max_subjects_per_document = 1;
  const fr::Status status = fr::ValidateSubmission(submission, limits);
  FR_CHECK(!status.ok());
}

FR_TEST(policy, canonicalisation_sorts_and_deduplicates) {
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.unmanaged_attributes = {RequireAttribute("zeta"), RequireAttribute("alpha"),
                                 RequireAttribute("alpha")};
  const fr::Status status = fr::CanonicalisePolicy(policy, fr::RuntimeLimits{});
  FR_CHECK(status.ok());
  FR_CHECK_EQ(policy.unmanaged_attributes.size(), std::size_t(2));
  FR_CHECK_EQ(policy.unmanaged_attributes[0].str(), std::string("alpha"));
  FR_CHECK_EQ(policy.unmanaged_attributes[1].str(), std::string("zeta"));
  FR_CHECK(fr::IsAttributeUnmanaged(policy, RequireAttribute("alpha")));
  FR_CHECK(!fr::IsAttributeUnmanaged(policy, RequireAttribute("beta")));
}

FR_TEST(policy, digest_and_codec_round_trip) {
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.version = fr::PolicyVersion(7);
  policy.observe_only_attributes = {RequireAttribute("mtu")};
  FR_CHECK(fr::CanonicalisePolicy(policy, fr::RuntimeLimits{}).ok());
  const std::string encoded = fr::EncodePolicy(policy);
  fr::ReconciliationPolicy decoded;
  FR_CHECK(fr::DecodePolicyExact(encoded, decoded, fr::RuntimeLimits{}));
  FR_CHECK(decoded == policy);
  FR_CHECK(fr::ComputePolicyDigest(decoded) == fr::ComputePolicyDigest(policy));

  std::string tampered = encoded;
  tampered[10] = static_cast<char>(tampered[10] ^ 0x02);
  FR_CHECK(!fr::DecodePolicyExact(tampered, decoded, fr::RuntimeLimits{}));
}

FR_TEST(policy, rejects_invalid_input) {
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.id = fr::PolicyId();
  FR_CHECK(!fr::CanonicalisePolicy(policy, fr::RuntimeLimits{}).ok());
  policy = fr::DefaultPolicy();
  policy.freshness.max_observation_age_ms = -1;
  FR_CHECK(!fr::CanonicalisePolicy(policy, fr::RuntimeLimits{}).ok());
}

FR_TEST(limits, evidence_class_text_round_trip) {
  const fr::EvidenceClass values[] = {fr::EvidenceClass::Real, fr::EvidenceClass::Synthetic,
                                      fr::EvidenceClass::Unsupported, fr::EvidenceClass::Unknown};
  for (const fr::EvidenceClass value : values) {
    fr::EvidenceClass parsed = fr::EvidenceClass::Unknown;
    FR_CHECK(fr::TryParseEvidenceClass(fr::ToText(value), parsed));
    FR_CHECK(parsed == value);
  }
  fr::EvidenceClass parsed = fr::EvidenceClass::Unknown;
  FR_CHECK(!fr::TryParseEvidenceClass("FRESH", parsed));
  FR_CHECK(!fr::TryParseEvidenceClass(nullptr, parsed));
}

FR_TEST(reason, every_enumerator_has_a_stable_name) {
  for (std::uint16_t raw = 0; raw <= 900; ++raw) {
    const fr::ReasonCode code = static_cast<fr::ReasonCode>(raw);
    const char* text = fr::ToText(code);
    FR_CHECK(text != nullptr);
    if (std::string(text) == "UNKNOWN_REASON") {
      // Only values outside the declared vocabulary may be unnamed.
      FR_CHECK(raw != 0);
    }
  }
  FR_CHECK_EQ(std::string(fr::ToText(fr::ReasonCode::None)), std::string("NONE"));
  FR_CHECK(fr::IsDriftReason(fr::ReasonCode::ClassifiedSubjectMissing));
  FR_CHECK(!fr::IsDriftReason(fr::ReasonCode::ScopeFenced));
  FR_CHECK(fr::IsAuthorityReason(fr::ReasonCode::ScopeFenced));
}

FR_TEST(status, string_form_is_deterministic) {
  const fr::Status ok;
  FR_CHECK(ok.ok());
  FR_CHECK_EQ(ok.ToString(), std::string("OK(NONE)"));
  const fr::Status failed(fr::StatusCode::Rejected, fr::ReasonCode::MalformedPayload, "why");
  FR_CHECK_EQ(failed.ToString(), std::string("REJECTED(MALFORMED_PAYLOAD: why)"));
}
