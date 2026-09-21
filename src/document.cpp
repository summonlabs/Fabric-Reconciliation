// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/document.hpp"

#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

Status ValidateState(const SubjectState& state, const RuntimeLimits& limits, const char* what) {
  if (state.size() > limits.max_attributes_per_subject) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  std::string(what) + " declares more attributes than the configured bound");
  }
  for (const auto& attribute : state) {
    if (attribute.first.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    std::string(what) + " carries an empty attribute key");
    }
    if (!attribute.second.IsValid()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    std::string(what) + " carries an invalid attribute value");
    }
  }
  return Status::Ok();
}

void EncodeState(CanonicalWriter& writer, const SubjectState& state) noexcept {
  writer.PutU32(static_cast<std::uint32_t>(state.size() & 0xffffffffu));
  for (const auto& attribute : state) {
    writer.PutBytes(attribute.first.str());
    attribute.second.Encode(writer);
  }
}

bool DecodeState(CanonicalReader& reader, SubjectState& out, const RuntimeLimits& limits) {
  std::uint32_t count = 0;
  if (!reader.ReadU32(count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > limits.max_attributes_per_subject) {
    return false;
  }
  SubjectState state;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string key_text;
    if (!reader.ReadBytes(key_text)) {
      return false;
    }
    const auto key = AttributeKey::TryParse(key_text);
    if (!key.has_value()) {
      return false;
    }
    AttributeValue value;
    if (!AttributeValue::Decode(reader, value)) {
      return false;
    }
    if (!state.emplace(*key, std::move(value)).second) {
      return false;
    }
  }
  out = std::move(state);
  return true;
}

}  // namespace

Sha256Digest ComputeIntentDigest(const IntentDocument& document) noexcept {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(document.intent.str());
  writer.PutBytes(document.scope.str());
  writer.PutBytes(document.definition.str());
  writer.PutU64(document.generation.value());
  writer.PutU64(document.policy_version.value());
  writer.PutU8(static_cast<std::uint8_t>(document.evidence));
  writer.PutBool(document.complete);
  writer.PutU32(static_cast<std::uint32_t>(document.subjects.size() & 0xffffffffu));
  for (const auto& entry : document.subjects) {
    writer.PutBytes(entry.first.str());
    writer.PutBool(entry.second.complete_attributes);
    EncodeState(writer, entry.second.desired);
  }
  return writer.Digest();
}

Sha256Digest ComputeObservationDigest(const ObservationDocument& document) noexcept {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(document.observation.str());
  writer.PutBytes(document.scope.str());
  writer.PutBytes(document.reporter.str());
  writer.PutU64(document.generation.value());
  writer.PutU8(static_cast<std::uint8_t>(document.evidence));
  writer.PutBool(document.complete);
  writer.PutBool(document.authoritative_absence);
  writer.PutU64(document.received_epoch.value());
  writer.PutI64(document.received_unix_ms);
  writer.PutU32(static_cast<std::uint32_t>(document.subjects.size() & 0xffffffffu));
  for (const auto& entry : document.subjects) {
    writer.PutBytes(entry.first.str());
    writer.PutBool(entry.second.complete_attributes);
    EncodeState(writer, entry.second.observed);
  }
  return writer.Digest();
}

Status ValidateIntent(const IntentDocument& document, const RuntimeLimits& limits) {
  if (document.intent.empty() || document.scope.empty() || document.definition.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "intent requires non-empty intent, scope and definition identities");
  }
  if (document.subjects.size() > limits.max_subjects_per_document) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  "intent declares more subjects than the configured bound");
  }
  for (const auto& entry : document.subjects) {
    if (entry.first.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "intent carries an empty subject identity");
    }
    const Status status = ValidateState(entry.second.desired, limits, "intent subject");
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Status ValidateObservation(const ObservationDocument& document, const RuntimeLimits& limits) {
  if (document.observation.empty() || document.scope.empty() || document.reporter.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "observation requires non-empty observation, scope and reporter identities");
  }
  if (document.subjects.size() > limits.max_subjects_per_document) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  "observation declares more subjects than the configured bound");
  }
  for (const auto& entry : document.subjects) {
    if (entry.first.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "observation carries an empty subject identity");
    }
    const Status status = ValidateState(entry.second.observed, limits, "observation subject");
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Status ValidateSubmission(const ObservationSubmission& submission, const RuntimeLimits& limits) {
  if (submission.observation.empty() || submission.scope.empty() || submission.reporter.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "observation requires non-empty observation, scope and reporter identities");
  }
  if (submission.subjects.size() > limits.max_subjects_per_document) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  "observation declares more subjects than the configured bound");
  }
  for (const auto& entry : submission.subjects) {
    if (entry.first.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "observation carries an empty subject identity");
    }
    const Status status = ValidateState(entry.second.observed, limits, "observation subject");
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

std::string EncodeIntent(const IntentDocument& document) {
  CanonicalWriter writer;
  writer.PutBytes(document.intent.str());
  writer.PutBytes(document.scope.str());
  writer.PutBytes(document.definition.str());
  writer.PutU64(document.generation.value());
  writer.PutU64(document.policy_version.value());
  writer.PutU8(static_cast<std::uint8_t>(document.evidence));
  writer.PutBool(document.complete);
  writer.PutU32(static_cast<std::uint32_t>(document.subjects.size() & 0xffffffffu));
  for (const auto& entry : document.subjects) {
    writer.PutBytes(entry.first.str());
    writer.PutBool(entry.second.complete_attributes);
    EncodeState(writer, entry.second.desired);
  }
  writer.PutDigest(document.digest);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

std::string EncodeObservation(const ObservationDocument& document) {
  CanonicalWriter writer;
  writer.PutBytes(document.observation.str());
  writer.PutBytes(document.scope.str());
  writer.PutBytes(document.reporter.str());
  writer.PutU64(document.generation.value());
  writer.PutU8(static_cast<std::uint8_t>(document.evidence));
  writer.PutBool(document.complete);
  writer.PutBool(document.authoritative_absence);
  writer.PutU64(document.received_epoch.value());
  writer.PutI64(document.received_unix_ms);
  writer.PutU32(static_cast<std::uint32_t>(document.subjects.size() & 0xffffffffu));
  for (const auto& entry : document.subjects) {
    writer.PutBytes(entry.first.str());
    writer.PutBool(entry.second.complete_attributes);
    EncodeState(writer, entry.second.observed);
  }
  writer.PutDigest(document.digest);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

std::string EncodeSubmission(const ObservationSubmission& submission) {
  CanonicalWriter writer;
  writer.PutBytes(submission.observation.str());
  writer.PutBytes(submission.scope.str());
  writer.PutBytes(submission.reporter.str());
  writer.PutU64(submission.generation.value());
  writer.PutU8(static_cast<std::uint8_t>(submission.evidence));
  writer.PutBool(submission.complete);
  writer.PutBool(submission.authoritative_absence);
  writer.PutU32(static_cast<std::uint32_t>(submission.subjects.size() & 0xffffffffu));
  for (const auto& entry : submission.subjects) {
    writer.PutBytes(entry.first.str());
    writer.PutBool(entry.second.complete_attributes);
    EncodeState(writer, entry.second.observed);
  }
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

bool DecodeIntent(CanonicalReader& reader, IntentDocument& out, const RuntimeLimits& limits) {
  IntentDocument document;
  std::string intent_text;
  std::string scope_text;
  std::string definition_text;
  std::uint64_t generation = 0;
  std::uint64_t policy_version = 0;
  std::uint8_t evidence = 0;
  bool complete = false;
  std::uint32_t count = 0;
  if (!reader.ReadBytes(intent_text) || !reader.ReadBytes(scope_text) ||
      !reader.ReadBytes(definition_text) || !reader.ReadU64(generation) ||
      !reader.ReadU64(policy_version) || !reader.ReadU8(evidence) || !reader.ReadBool(complete) ||
      !reader.ReadU32(count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > limits.max_subjects_per_document) {
    return false;
  }
  if (evidence > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return false;
  }
  const auto intent = IntentId::TryParse(intent_text);
  const auto scope = ScopeId::TryParse(scope_text);
  const auto definition = DefinitionId::TryParse(definition_text);
  if (!intent.has_value() || !scope.has_value() || !definition.has_value()) {
    return false;
  }
  document.intent = *intent;
  document.scope = *scope;
  document.definition = *definition;
  document.generation = IntentGeneration(generation);
  document.policy_version = PolicyVersion(policy_version);
  document.evidence = static_cast<EvidenceClass>(evidence);
  document.complete = complete;

  for (std::uint32_t index = 0; index < count; ++index) {
    std::string subject_text;
    bool attributes_complete = false;
    if (!reader.ReadBytes(subject_text) || !reader.ReadBool(attributes_complete)) {
      return false;
    }
    const auto subject = SubjectId::TryParse(subject_text);
    if (!subject.has_value()) {
      return false;
    }
    SubjectIntent entry;
    entry.id = *subject;
    entry.complete_attributes = attributes_complete;
    if (!DecodeState(reader, entry.desired, limits)) {
      return false;
    }
    if (!document.subjects.emplace(*subject, std::move(entry)).second) {
      return false;
    }
  }

  Sha256Digest digest{};
  if (!reader.ReadDigest(digest)) {
    return false;
  }
  if (digest != ComputeIntentDigest(document)) {
    return false;
  }
  document.digest = digest;
  out = std::move(document);
  return true;
}

bool DecodeObservation(CanonicalReader& reader, ObservationDocument& out,
                       const RuntimeLimits& limits) {
  ObservationDocument document;
  std::string observation_text;
  std::string scope_text;
  std::string reporter_text;
  std::uint64_t generation = 0;
  std::uint8_t evidence = 0;
  bool complete = false;
  bool authoritative_absence = false;
  std::uint64_t received_epoch = 0;
  std::int64_t received_unix_ms = 0;
  std::uint32_t count = 0;
  if (!reader.ReadBytes(observation_text) || !reader.ReadBytes(scope_text) ||
      !reader.ReadBytes(reporter_text) || !reader.ReadU64(generation) ||
      !reader.ReadU8(evidence) || !reader.ReadBool(complete) ||
      !reader.ReadBool(authoritative_absence) || !reader.ReadU64(received_epoch) ||
      !reader.ReadI64(received_unix_ms) || !reader.ReadU32(count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > limits.max_subjects_per_document) {
    return false;
  }
  if (evidence > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return false;
  }
  const auto observation = ObservationId::TryParse(observation_text);
  const auto scope = ScopeId::TryParse(scope_text);
  const auto reporter = ReporterId::TryParse(reporter_text);
  if (!observation.has_value() || !scope.has_value() || !reporter.has_value()) {
    return false;
  }
  document.observation = *observation;
  document.scope = *scope;
  document.reporter = *reporter;
  document.generation = ObservationGeneration(generation);
  document.evidence = static_cast<EvidenceClass>(evidence);
  document.complete = complete;
  document.authoritative_absence = authoritative_absence;
  document.received_epoch = CoordinatorEpoch(received_epoch);
  document.received_unix_ms = received_unix_ms;

  for (std::uint32_t index = 0; index < count; ++index) {
    std::string subject_text;
    bool attributes_complete = false;
    if (!reader.ReadBytes(subject_text) || !reader.ReadBool(attributes_complete)) {
      return false;
    }
    const auto subject = SubjectId::TryParse(subject_text);
    if (!subject.has_value()) {
      return false;
    }
    SubjectObservation entry;
    entry.id = *subject;
    entry.complete_attributes = attributes_complete;
    if (!DecodeState(reader, entry.observed, limits)) {
      return false;
    }
    if (!document.subjects.emplace(*subject, std::move(entry)).second) {
      return false;
    }
  }

  Sha256Digest digest{};
  if (!reader.ReadDigest(digest)) {
    return false;
  }
  if (digest != ComputeObservationDigest(document)) {
    return false;
  }
  document.digest = digest;
  out = std::move(document);
  return true;
}

bool DecodeSubmission(CanonicalReader& reader, ObservationSubmission& out,
                      const RuntimeLimits& limits) {
  ObservationSubmission submission;
  std::string observation_text;
  std::string scope_text;
  std::string reporter_text;
  std::uint64_t generation = 0;
  std::uint8_t evidence = 0;
  bool complete = false;
  bool authoritative_absence = false;
  std::uint32_t count = 0;
  if (!reader.ReadBytes(observation_text) || !reader.ReadBytes(scope_text) ||
      !reader.ReadBytes(reporter_text) || !reader.ReadU64(generation) ||
      !reader.ReadU8(evidence) || !reader.ReadBool(complete) ||
      !reader.ReadBool(authoritative_absence) || !reader.ReadU32(count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > limits.max_subjects_per_document) {
    return false;
  }
  if (evidence > static_cast<std::uint8_t>(EvidenceClass::Unsupported)) {
    return false;
  }
  const auto observation = ObservationId::TryParse(observation_text);
  const auto scope = ScopeId::TryParse(scope_text);
  const auto reporter = ReporterId::TryParse(reporter_text);
  if (!observation.has_value() || !scope.has_value() || !reporter.has_value()) {
    return false;
  }
  submission.observation = *observation;
  submission.scope = *scope;
  submission.reporter = *reporter;
  submission.generation = ObservationGeneration(generation);
  submission.evidence = static_cast<EvidenceClass>(evidence);
  submission.complete = complete;
  submission.authoritative_absence = authoritative_absence;

  for (std::uint32_t index = 0; index < count; ++index) {
    std::string subject_text;
    bool attributes_complete = false;
    if (!reader.ReadBytes(subject_text) || !reader.ReadBool(attributes_complete)) {
      return false;
    }
    const auto subject = SubjectId::TryParse(subject_text);
    if (!subject.has_value()) {
      return false;
    }
    SubjectObservation entry;
    entry.id = *subject;
    entry.complete_attributes = attributes_complete;
    if (!DecodeState(reader, entry.observed, limits)) {
      return false;
    }
    if (!submission.subjects.emplace(*subject, std::move(entry)).second) {
      return false;
    }
  }
  out = std::move(submission);
  return true;
}

bool DecodeIntentExact(std::string_view bytes, IntentDocument& out, const RuntimeLimits& limits) {
  CanonicalReader reader(bytes);
  if (!DecodeIntent(reader, out, limits)) {
    return false;
  }
  return reader.AtEnd();
}

bool DecodeObservationExact(std::string_view bytes, ObservationDocument& out,
                            const RuntimeLimits& limits) {
  CanonicalReader reader(bytes);
  if (!DecodeObservation(reader, out, limits)) {
    return false;
  }
  return reader.AtEnd();
}

bool DecodeSubmissionExact(std::string_view bytes, ObservationSubmission& out,
                           const RuntimeLimits& limits) {
  CanonicalReader reader(bytes);
  if (!DecodeSubmission(reader, out, limits)) {
    return false;
  }
  return reader.AtEnd();
}

}  // namespace fabric_reconciliation
}  // namespace summon
