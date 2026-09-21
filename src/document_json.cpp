// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/document_json.hpp"

#include "summon/fabric_reconciliation/json.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

using json::Value;

Status Reject(const char* what) {
  return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput, std::string("document: ") + what);
}

Value DigestJson(const Sha256Digest& digest) { return Value::Text(ToHex(digest)); }

Value StateJson(const SubjectState& state) {
  Value attributes = Value::Object();
  for (const auto& attribute : state) {
    Value value = Value::Object();
    switch (attribute.second.kind()) {
      case AttributeKind::Absent: value.Set("absent", Value::Boolean(true)); break;
      case AttributeKind::Boolean:
        value.Set("bool", Value::Boolean(attribute.second.bool_value()));
        break;
      case AttributeKind::Integer:
        value.Set("i64", Value::Integer(attribute.second.int_value()));
        break;
      case AttributeKind::Unsigned:
        value.Set("u64", Value::Unsigned(attribute.second.uint_value()));
        break;
      case AttributeKind::Text:
        value.Set("text", Value::Text(attribute.second.text_value()));
        break;
      case AttributeKind::Token:
        value.Set("token", Value::Text(attribute.second.text_value()));
        break;
      case AttributeKind::Invalid:
      default:
        value.Set("invalid", Value::Boolean(true));
        break;
    }
    attributes.Set(attribute.first.str(), std::move(value));
  }
  return attributes;
}

Status ParseState(const Value& node, SubjectState& out, const RuntimeLimits& limits) {
  if (node.IsNull()) {
    return Status::Ok();
  }
  if (!node.IsObject()) {
    return Reject("subject attributes must be an object");
  }
  if (node.size() > limits.max_attributes_per_subject) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  "subject declares more attributes than the configured bound");
  }
  SubjectState state;
  for (const auto& member : node.members()) {
    const auto key = AttributeKey::TryParse(member.first);
    if (!key.has_value()) {
      return Reject("invalid attribute key");
    }
    if (!member.second.IsObject()) {
      return Reject("attribute value must be an object");
    }
    AttributeValue value;
    bool resolved = false;
    if (const Value* absent = member.second.Find("absent"); absent != nullptr) {
      if (!absent->AsBool(false)) {
        return Reject("absent marker must be true");
      }
      value = AttributeValue::Absent();
      resolved = true;
    }
    if (const Value* flag = member.second.Find("bool"); flag != nullptr) {
      value = AttributeValue::Boolean(flag->AsBool(false));
      resolved = true;
    }
    if (const Value* number = member.second.Find("i64"); number != nullptr) {
      value = AttributeValue::Integer(number->AsInteger(0));
      resolved = true;
    }
    if (const Value* number = member.second.Find("u64"); number != nullptr) {
      value = AttributeValue::Unsigned(number->AsUnsigned(0));
      resolved = true;
    }
    if (const Value* text = member.second.Find("text"); text != nullptr) {
      value = AttributeValue::Text(text->AsText());
      resolved = true;
    }
    if (const Value* token = member.second.Find("token"); token != nullptr) {
      value = AttributeValue::Token(token->AsText());
      resolved = true;
    }
    if (!resolved) {
      return Reject("attribute value does not name exactly one of absent/bool/i64/u64/text/token");
    }
    if (!state.emplace(*key, std::move(value)).second) {
      return Reject("duplicate attribute key");
    }
  }
  out = std::move(state);
  return Status::Ok();
}

Status ReadU64(const Value& object, const char* key, std::uint64_t& out) {
  const Value* member = object.Find(key);
  if (member == nullptr) {
    return Status::Ok();
  }
  if (!member->IsNumber()) {
    return Reject("expected an integer member");
  }
  out = member->AsUnsigned(0);
  return Status::Ok();
}

Status ReadBool(const Value& object, const char* key, bool& out) {
  const Value* member = object.Find(key);
  if (member == nullptr) {
    return Status::Ok();
  }
  if (member->kind() != json::Kind::Boolean) {
    return Reject("expected a boolean member");
  }
  out = member->AsBool(false);
  return Status::Ok();
}

Status ReadText(const Value& object, const char* key, std::string& out) {
  const Value* member = object.Find(key);
  if (member == nullptr) {
    return Status::Ok();
  }
  if (!member->IsText()) {
    return Reject("expected a text member");
  }
  out = member->AsText();
  return Status::Ok();
}

Status CheckMembers(const Value& object, const std::vector<const char*>& allowed) {
  if (!object.IsObject()) {
    return Reject("expected a top level object");
  }
  for (const auto& member : object.members()) {
    bool known = false;
    for (const char* name : allowed) {
      if (member.first == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "unknown member '" + member.first + "'");
    }
  }
  return Status::Ok();
}

EvidenceClass ParseEvidence(const Value& object) {
  const Value* member = object.Find("evidence");
  if (member == nullptr || !member->IsText()) {
    return EvidenceClass::Synthetic;
  }
  EvidenceClass parsed = EvidenceClass::Synthetic;
  if (TryParseEvidenceClass(member->AsText().c_str(), parsed)) {
    return parsed;
  }
  return EvidenceClass::Synthetic;
}

Value EvidenceJson(EvidenceClass value) { return Value::Text(ToText(value)); }

}  // namespace

std::string ToJson(const IntentDocument& document) {
  Value root = Value::Object();
  root.Set("intent", Value::Text(document.intent.str()));
  root.Set("scope", Value::Text(document.scope.str()));
  root.Set("definition", Value::Text(document.definition.str()));
  root.Set("generation", Value::Unsigned(document.generation.value()));
  root.Set("policy_version", Value::Unsigned(document.policy_version.value()));
  root.Set("evidence", EvidenceJson(document.evidence));
  root.Set("complete", Value::Boolean(document.complete));
  root.Set("digest", DigestJson(document.digest));
  Value subjects = Value::Object();
  for (const auto& entry : document.subjects) {
    Value subject = Value::Object();
    subject.Set("complete_attributes", Value::Boolean(entry.second.complete_attributes));
    subject.Set("attributes", StateJson(entry.second.desired));
    subjects.Set(entry.first.str(), std::move(subject));
  }
  root.Set("subjects", std::move(subjects));
  return root.DumpCanonical(2);
}

std::string ToJson(const ObservationSubmission& submission) {
  Value root = Value::Object();
  root.Set("observation", Value::Text(submission.observation.str()));
  root.Set("scope", Value::Text(submission.scope.str()));
  root.Set("reporter", Value::Text(submission.reporter.str()));
  root.Set("generation", Value::Unsigned(submission.generation.value()));
  root.Set("evidence", EvidenceJson(submission.evidence));
  root.Set("complete", Value::Boolean(submission.complete));
  root.Set("authoritative_absence", Value::Boolean(submission.authoritative_absence));
  Value subjects = Value::Object();
  for (const auto& entry : submission.subjects) {
    Value subject = Value::Object();
    subject.Set("complete_attributes", Value::Boolean(entry.second.complete_attributes));
    subject.Set("attributes", StateJson(entry.second.observed));
    subjects.Set(entry.first.str(), std::move(subject));
  }
  root.Set("subjects", std::move(subjects));
  return root.DumpCanonical(2);
}

std::string ToJson(const ObservationDocument& document) {
  Value root = Value::Object();
  root.Set("observation", Value::Text(document.observation.str()));
  root.Set("scope", Value::Text(document.scope.str()));
  root.Set("reporter", Value::Text(document.reporter.str()));
  root.Set("generation", Value::Unsigned(document.generation.value()));
  root.Set("evidence", EvidenceJson(document.evidence));
  root.Set("complete", Value::Boolean(document.complete));
  root.Set("authoritative_absence", Value::Boolean(document.authoritative_absence));
  root.Set("received_epoch", Value::Unsigned(document.received_epoch.value()));
  root.Set("received_unix_ms", Value::Integer(document.received_unix_ms));
  root.Set("digest", DigestJson(document.digest));
  Value subjects = Value::Object();
  for (const auto& entry : document.subjects) {
    Value subject = Value::Object();
    subject.Set("complete_attributes", Value::Boolean(entry.second.complete_attributes));
    subject.Set("attributes", StateJson(entry.second.observed));
    subjects.Set(entry.first.str(), std::move(subject));
  }
  root.Set("subjects", std::move(subjects));
  return root.DumpCanonical(2);
}

std::string ToJson(const ReconciliationPolicy& policy) {
  Value root = Value::Object();
  root.Set("id", Value::Text(policy.id.str()));
  root.Set("version", Value::Unsigned(policy.version.value()));
  root.Set("max_observation_age_ms", Value::Integer(policy.freshness.max_observation_age_ms));
  root.Set("require_current_epoch", Value::Boolean(policy.freshness.require_current_epoch));
  root.Set("require_complete_coverage_for_absence",
           Value::Boolean(policy.freshness.require_complete_coverage_for_absence));
  root.Set("permit_apply_missing", Value::Boolean(policy.permit_apply_missing));
  root.Set("permit_apply_mismatched", Value::Boolean(policy.permit_apply_mismatched));
  root.Set("permit_withdraw_unexpected", Value::Boolean(policy.permit_withdraw_unexpected));
  root.Set("require_complete_coverage_for_convergence_proof",
           Value::Boolean(policy.require_complete_coverage_for_convergence_proof));
  root.Set("require_independent_verification",
           Value::Boolean(policy.require_independent_verification));
  root.Set("digest", DigestJson(ComputePolicyDigest(policy)));
  Value unmanaged = Value::Array();
  for (const AttributeKey& key : policy.unmanaged_attributes) {
    unmanaged.Push(Value::Text(key.str()));
  }
  root.Set("unmanaged_attributes", std::move(unmanaged));
  Value observe_only = Value::Array();
  for (const AttributeKey& key : policy.observe_only_attributes) {
    observe_only.Push(Value::Text(key.str()));
  }
  root.Set("observe_only_attributes", std::move(observe_only));
  return root.DumpCanonical(2);
}

std::string ToJson(const ClassificationReport& report) {
  Value root = Value::Object();
  root.Set("scope", Value::Text(report.scope.str()));
  root.Set("has_intent", Value::Boolean(report.evidence.has_intent));
  root.Set("has_observation", Value::Boolean(report.evidence.has_observation));
  root.Set("freshness", Value::Text(ToText(report.evidence.freshness)));
  root.Set("subject_visits", Value::Unsigned(report.subject_visits));
  root.Set("attribute_comparisons", Value::Unsigned(report.attribute_comparisons));
  Value records = Value::Array();
  for (const DriftRecord& record : report.records) {
    Value item = Value::Object();
    item.Set("subject", Value::Text(record.subject.str()));
    item.Set("drift", Value::Text(ToText(record.drift)));
    item.Set("reason", Value::Text(ToText(record.reason)));
    Value deltas = Value::Array();
    for (const AttributeDelta& delta : record.deltas) {
      Value entry = Value::Object();
      entry.Set("key", Value::Text(delta.key.str()));
      entry.Set("kind", Value::Text(ToText(delta.kind)));
      entry.Set("intended", Value::Text(delta.intended.Render()));
      entry.Set("observed", Value::Text(delta.observed.Render()));
      deltas.Push(std::move(entry));
    }
    item.Set("deltas", std::move(deltas));
    records.Push(std::move(item));
  }
  root.Set("records", std::move(records));
  return root.DumpCanonical(2);
}

std::string ToJson(const ReconciliationPlan& plan) {
  Value root = Value::Object();
  root.Set("plan_id", Value::Text(plan.plan_id));
  root.Set("scope", Value::Text(plan.scope.str()));
  root.Set("definition", Value::Text(plan.definition.str()));
  root.Set("epoch", Value::Unsigned(plan.epoch.value()));
  root.Set("boot", Value::Text(plan.boot.ToHex()));
  root.Set("policy", Value::Text(plan.policy_id.str()));
  root.Set("policy_version", Value::Unsigned(plan.policy_version.value()));
  root.Set("policy_digest", DigestJson(plan.policy_digest));
  root.Set("intent", Value::Text(plan.intent.str()));
  root.Set("intent_generation", Value::Unsigned(plan.intent_generation.value()));
  root.Set("intent_digest", DigestJson(plan.intent_digest));
  root.Set("observation", Value::Text(plan.observation.str()));
  root.Set("observation_generation", Value::Unsigned(plan.observation_generation.value()));
  root.Set("observation_reporter", Value::Text(plan.reporter.str()));
  root.Set("observation_digest", DigestJson(plan.observation_digest));
  root.Set("observation_epoch", Value::Unsigned(plan.observation_epoch.value()));
  root.Set("observation_received_unix_ms", Value::Integer(plan.observation_received_unix_ms));
  root.Set("freshness", Value::Text(ToText(plan.freshness)));
  root.Set("verdict", Value::Text(ToText(plan.verdict)));
  root.Set("scope_reason", Value::Text(ToText(plan.scope_reason)));
  root.Set("evidence", EvidenceJson(plan.evidence));
  root.Set("truncated", Value::Boolean(plan.truncated));
  root.Set("mutation_count", Value::Unsigned(plan.MutationCount()));
  Value scope_explanation = Value::Array();
  for (const ExplanationStep& step : plan.scope_explanation.steps()) {
    Value item = Value::Object();
    item.Set("code", Value::Text(ToText(step.code)));
    item.Set("detail", Value::Text(step.detail));
    scope_explanation.Push(std::move(item));
  }
  root.Set("scope_explanation", std::move(scope_explanation));
  Value decisions = Value::Array();
  for (const Decision& decision : plan.decisions) {
    Value item = Value::Object();
    item.Set("subject", Value::Text(decision.subject.str()));
    item.Set("drift", Value::Text(ToText(decision.drift)));
    item.Set("reason", Value::Text(ToText(decision.reason)));
    item.Set("action", Value::Text(ToText(decision.action)));
    item.Set("disposition", Value::Text(ToText(decision.disposition)));
    item.Set("idempotency_key", DigestJson(decision.idempotency_key));
    item.Set("authority", Value::Text(decision.authority.Render()));
    Value steps = Value::Array();
    for (const ExplanationStep& step : decision.explanation.steps()) {
      Value entry = Value::Object();
      entry.Set("code", Value::Text(ToText(step.code)));
      entry.Set("detail", Value::Text(step.detail));
      steps.Push(std::move(entry));
    }
    item.Set("explanation", std::move(steps));
    decisions.Push(std::move(item));
  }
  root.Set("decisions", std::move(decisions));
  return root.DumpCanonical(2);
}

std::string ToJson(const BootReport& report) {
  Value root = Value::Object();
  root.Set("store", Value::Text(report.store.ToHex()));
  root.Set("boot", Value::Text(report.boot.ToHex()));
  root.Set("epoch", Value::Unsigned(report.epoch.value()));
  root.Set("previous_epoch", Value::Unsigned(report.previous_epoch.value()));
  root.Set("store_created", Value::Boolean(report.store_created));
  root.Set("snapshot_loaded", Value::Boolean(report.snapshot_loaded));
  root.Set("recovered_torn_tail", Value::Boolean(report.recovered_torn_tail));
  root.Set("recovered_bytes", Value::Unsigned(report.recovered_bytes));
  root.Set("records_replayed", Value::Unsigned(report.records_replayed));
  root.Set("attempts_restored", Value::Unsigned(report.attempts_restored));
  root.Set("attempts_interrupted", Value::Unsigned(report.attempts_interrupted));
  root.Set("attempts_abandoned", Value::Unsigned(report.attempts_abandoned));
  root.Set("attempts_fenced", Value::Unsigned(report.attempts_fenced));
  root.Set("observations_retained", Value::Unsigned(report.observations_retained));
  root.Set("outcomes_retained", Value::Unsigned(report.outcomes_retained));
  root.Set("fences_active", Value::Unsigned(report.fences_active));
  root.Set("freshness_restored", Value::Boolean(report.freshness_restored));
  root.Set("mutation_authority_restored", Value::Boolean(report.mutation_authority_restored));
  Value notes = Value::Array();
  for (const ReasonCode note : report.notes) {
    notes.Push(Value::Text(ToText(note)));
  }
  root.Set("notes", std::move(notes));
  return root.DumpCanonical(2);
}

std::string ToJson(const ReconciliationOutcome& outcome) {
  Value root = Value::Object();
  root.Set("idempotency_key", DigestJson(outcome.idempotency_key));
  root.Set("scope", Value::Text(outcome.scope.str()));
  root.Set("subject", Value::Text(outcome.subject.str()));
  root.Set("action", Value::Text(ToText(outcome.action)));
  root.Set("attempt", Value::Unsigned(outcome.attempt.value()));
  root.Set("epoch", Value::Unsigned(outcome.epoch.value()));
  root.Set("boot", Value::Text(outcome.boot.ToHex()));
  root.Set("terminal_state", Value::Text(ToText(outcome.terminal_state)));
  root.Set("reason", Value::Text(ToText(outcome.reason)));
  root.Set("committed_unix_ms", Value::Integer(outcome.committed_unix_ms));
  root.Set("verified", Value::Boolean(outcome.verified));
  root.Set("verified_generation", Value::Unsigned(outcome.verified_generation.value()));
  root.Set("ambiguous", Value::Boolean(outcome.ambiguous));
  return root.DumpCanonical(2);
}

std::string ToJson(const FenceEntry& fence) {
  Value root = Value::Object();
  root.Set("id", Value::Text(fence.id.str()));
  root.Set("scope", Value::Text(fence.scope.str()));
  root.Set("subject", Value::Text(fence.subject.str()));
  root.Set("fenced_below", Value::Unsigned(fence.fenced_below.value()));
  root.Set("reason", Value::Text(ToText(fence.reason)));
  root.Set("detail", Value::Text(fence.detail));
  root.Set("created_unix_ms", Value::Integer(fence.created_unix_ms));
  return root.DumpCanonical(2);
}

std::string ToJson(const AttemptRecord& attempt) {
  Value root = Value::Object();
  root.Set("attempt", Value::Unsigned(attempt.id.value()));
  root.Set("idempotency_key", DigestJson(attempt.idempotency_key));
  root.Set("scope", Value::Text(attempt.scope.str()));
  root.Set("subject", Value::Text(attempt.subject.str()));
  root.Set("action", Value::Text(ToText(attempt.action)));
  root.Set("state", Value::Text(ToText(attempt.state)));
  root.Set("epoch", Value::Unsigned(attempt.epoch.value()));
  root.Set("boot", Value::Text(attempt.boot.ToHex()));
  root.Set("issued_unix_ms", Value::Integer(attempt.issued_unix_ms));
  Value transitions = Value::Array();
  for (const AttemptTransition& transition : attempt.transitions) {
    Value item = Value::Object();
    item.Set("from", Value::Text(ToText(transition.from)));
    item.Set("to", Value::Text(ToText(transition.to)));
    item.Set("epoch", Value::Unsigned(transition.epoch.value()));
    item.Set("reason", Value::Text(ToText(transition.reason)));
    transitions.Push(std::move(item));
  }
  root.Set("transitions", std::move(transitions));
  return root.DumpCanonical(2);
}

Status ParseJson(std::string_view text, IntentDocument& out, const RuntimeLimits& limits) {
  json::Value root;
  const Status parsed = json::Parse(text, json::ParseLimits{}, root);
  if (!parsed.ok()) {
    return parsed;
  }
  const Status shape = CheckMembers(root, {"intent", "scope", "definition", "generation",
                                           "policy_version", "evidence", "complete", "subjects"});
  if (!shape.ok()) {
    return shape;
  }
  std::string intent_text;
  std::string scope_text;
  std::string definition_text;
  std::uint64_t generation = 0;
  std::uint64_t policy_version = 0;
  bool complete = true;
  Status status = ReadText(root, "intent", intent_text);
  if (!status.ok()) return status;
  status = ReadText(root, "scope", scope_text);
  if (!status.ok()) return status;
  status = ReadText(root, "definition", definition_text);
  if (!status.ok()) return status;
  status = ReadU64(root, "generation", generation);
  if (!status.ok()) return status;
  status = ReadU64(root, "policy_version", policy_version);
  if (!status.ok()) return status;
  status = ReadBool(root, "complete", complete);
  if (!status.ok()) return status;

  const auto intent = IntentId::TryParse(intent_text);
  const auto scope = ScopeId::TryParse(scope_text);
  const auto definition = DefinitionId::TryParse(definition_text);
  if (!intent.has_value() || !scope.has_value() || !definition.has_value()) {
    return Reject("intent, scope and definition must be valid identities");
  }
  IntentDocument document;
  document.intent = *intent;
  document.scope = *scope;
  document.definition = *definition;
  document.generation = IntentGeneration(generation);
  document.policy_version = PolicyVersion(policy_version);
  document.evidence = ParseEvidence(root);
  document.complete = complete;

  const Value* subjects = root.Find("subjects");
  if (subjects != nullptr) {
    if (!subjects->IsObject()) {
      return Reject("subjects must be an object");
    }
    if (subjects->size() > limits.max_subjects_per_document) {
      return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                    "intent declares more subjects than the configured bound");
    }
    for (const auto& member : subjects->members()) {
      const auto subject = SubjectId::TryParse(member.first);
      if (!subject.has_value()) {
        return Reject("invalid subject identity");
      }
      if (!member.second.IsObject()) {
        return Reject("subject entry must be an object");
      }
      SubjectIntent entry;
      entry.id = *subject;
      Status entry_status = ReadBool(member.second, "complete_attributes", entry.complete_attributes);
      if (!entry_status.ok()) return entry_status;
      const Value* attributes = member.second.Find("attributes");
      if (attributes != nullptr) {
        entry_status = ParseState(*attributes, entry.desired, limits);
        if (!entry_status.ok()) return entry_status;
      }
      if (!document.subjects.emplace(*subject, std::move(entry)).second) {
        return Reject("duplicate subject");
      }
    }
  }
  document.digest = ComputeIntentDigest(document);
  const Status valid = ValidateIntent(document, limits);
  if (!valid.ok()) {
    return valid;
  }
  out = std::move(document);
  return Status::Ok();
}

Status ParseJson(std::string_view text, ObservationSubmission& out, const RuntimeLimits& limits) {
  json::Value root;
  const Status parsed = json::Parse(text, json::ParseLimits{}, root);
  if (!parsed.ok()) {
    return parsed;
  }
  const Status shape = CheckMembers(root, {"observation", "scope", "reporter", "generation",
                                           "evidence", "complete", "authoritative_absence",
                                           "subjects"});
  if (!shape.ok()) {
    return shape;
  }
  std::string observation_text;
  std::string scope_text;
  std::string reporter_text;
  std::uint64_t generation = 0;
  bool complete = true;
  bool absence = false;
  Status status = ReadText(root, "observation", observation_text);
  if (!status.ok()) return status;
  status = ReadText(root, "scope", scope_text);
  if (!status.ok()) return status;
  status = ReadText(root, "reporter", reporter_text);
  if (!status.ok()) return status;
  status = ReadU64(root, "generation", generation);
  if (!status.ok()) return status;
  status = ReadBool(root, "complete", complete);
  if (!status.ok()) return status;
  status = ReadBool(root, "authoritative_absence", absence);
  if (!status.ok()) return status;

  const auto observation = ObservationId::TryParse(observation_text);
  const auto scope = ScopeId::TryParse(scope_text);
  const auto reporter = ReporterId::TryParse(reporter_text);
  if (!observation.has_value() || !scope.has_value() || !reporter.has_value()) {
    return Reject("observation, scope and reporter must be valid identities");
  }
  ObservationSubmission submission;
  submission.observation = *observation;
  submission.scope = *scope;
  submission.reporter = *reporter;
  submission.generation = ObservationGeneration(generation);
  submission.evidence = ParseEvidence(root);
  submission.complete = complete;
  submission.authoritative_absence = absence;

  const Value* subjects = root.Find("subjects");
  if (subjects != nullptr) {
    if (!subjects->IsObject()) {
      return Reject("subjects must be an object");
    }
    if (subjects->size() > limits.max_subjects_per_document) {
      return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                    "observation declares more subjects than the configured bound");
    }
    for (const auto& member : subjects->members()) {
      const auto subject = SubjectId::TryParse(member.first);
      if (!subject.has_value()) {
        return Reject("invalid subject identity");
      }
      if (!member.second.IsObject()) {
        return Reject("subject entry must be an object");
      }
      SubjectObservation entry;
      entry.id = *subject;
      Status entry_status =
          ReadBool(member.second, "complete_attributes", entry.complete_attributes);
      if (!entry_status.ok()) return entry_status;
      const Value* attributes = member.second.Find("attributes");
      if (attributes != nullptr) {
        entry_status = ParseState(*attributes, entry.observed, limits);
        if (!entry_status.ok()) return entry_status;
      }
      if (!submission.subjects.emplace(*subject, std::move(entry)).second) {
        return Reject("duplicate subject");
      }
    }
  }
  const Status valid = ValidateSubmission(submission, limits);
  if (!valid.ok()) {
    return valid;
  }
  out = std::move(submission);
  return Status::Ok();
}

Status ParseJson(std::string_view text, ReconciliationPolicy& out, const RuntimeLimits& limits) {
  json::Value root;
  const Status parsed = json::Parse(text, json::ParseLimits{}, root);
  if (!parsed.ok()) {
    return parsed;
  }
  const Status shape = CheckMembers(
      root, {"id", "version", "max_observation_age_ms", "require_current_epoch",
             "require_complete_coverage_for_absence", "permit_apply_missing",
             "permit_apply_mismatched", "permit_withdraw_unexpected",
             "require_complete_coverage_for_convergence_proof",
             "require_independent_verification", "unmanaged_attributes",
             "observe_only_attributes"});
  if (!shape.ok()) {
    return shape;
  }
  ReconciliationPolicy policy = DefaultPolicy();
  std::string id_text;
  std::uint64_t version = 1;
  std::int64_t max_age = policy.freshness.max_observation_age_ms;
  Status status = ReadText(root, "id", id_text);
  if (!status.ok()) return status;
  status = ReadU64(root, "version", version);
  if (!status.ok()) return status;
  const Value* age = root.Find("max_observation_age_ms");
  if (age != nullptr) {
    if (!age->IsNumber()) {
      return Reject("max_observation_age_ms must be an integer");
    }
    max_age = age->AsInteger(0);
  }
  status = ReadBool(root, "require_current_epoch", policy.freshness.require_current_epoch);
  if (!status.ok()) return status;
  status = ReadBool(root, "require_complete_coverage_for_absence",
                    policy.freshness.require_complete_coverage_for_absence);
  if (!status.ok()) return status;
  status = ReadBool(root, "permit_apply_missing", policy.permit_apply_missing);
  if (!status.ok()) return status;
  status = ReadBool(root, "permit_apply_mismatched", policy.permit_apply_mismatched);
  if (!status.ok()) return status;
  status = ReadBool(root, "permit_withdraw_unexpected", policy.permit_withdraw_unexpected);
  if (!status.ok()) return status;
  status = ReadBool(root, "require_complete_coverage_for_convergence_proof",
                    policy.require_complete_coverage_for_convergence_proof);
  if (!status.ok()) return status;
  status = ReadBool(root, "require_independent_verification",
                    policy.require_independent_verification);
  if (!status.ok()) return status;

  if (!id_text.empty()) {
    const auto id = PolicyId::TryParse(id_text);
    if (!id.has_value()) {
      return Reject("invalid policy identity");
    }
    policy.id = *id;
  }
  policy.version = PolicyVersion(version);
  policy.freshness.max_observation_age_ms = max_age;

  for (const char* field : {"unmanaged_attributes", "observe_only_attributes"}) {
    const Value* list = root.Find(field);
    if (list == nullptr) {
      continue;
    }
    if (!list->IsArray()) {
      return Reject("attribute list must be an array");
    }
    std::vector<AttributeKey>& target = (std::string_view(field) == "unmanaged_attributes")
                                            ? policy.unmanaged_attributes
                                            : policy.observe_only_attributes;
    for (const Value& item : list->items()) {
      if (!item.IsText()) {
        return Reject("attribute list entries must be text");
      }
      const auto key = AttributeKey::TryParse(item.AsText());
      if (!key.has_value()) {
        return Reject("invalid attribute key in policy");
      }
      target.push_back(*key);
    }
  }
  const Status canonical = CanonicalisePolicy(policy, limits);
  if (!canonical.ok()) {
    return canonical;
  }
  out = std::move(policy);
  return Status::Ok();
}

Status ParseJson(std::string_view text, FenceEntry& out, const RuntimeLimits& limits) {
  (void)limits;
  json::Value root;
  const Status parsed = json::Parse(text, json::ParseLimits{}, root);
  if (!parsed.ok()) {
    return parsed;
  }
  const Status shape = CheckMembers(
      root, {"id", "scope", "subject", "fenced_below", "reason", "detail", "created_unix_ms"});
  if (!shape.ok()) {
    return shape;
  }
  std::string id_text;
  std::string scope_text;
  std::string subject_text;
  std::string reason_text;
  std::uint64_t fenced_below = 0;
  Status status = ReadText(root, "id", id_text);
  if (!status.ok()) return status;
  status = ReadText(root, "scope", scope_text);
  if (!status.ok()) return status;
  status = ReadText(root, "subject", subject_text);
  if (!status.ok()) return status;
  status = ReadText(root, "reason", reason_text);
  if (!status.ok()) return status;
  status = ReadU64(root, "fenced_below", fenced_below);
  if (!status.ok()) return status;

  const auto id = FenceId::TryParse(id_text);
  const auto scope = ScopeId::TryParse(scope_text);
  if (!id.has_value() || !scope.has_value()) {
    return Reject("fence id and scope must be valid identities");
  }
  FenceEntry fence;
  fence.id = *id;
  fence.scope = *scope;
  if (!subject_text.empty()) {
    const auto subject = SubjectId::TryParse(subject_text);
    if (!subject.has_value()) {
      return Reject("invalid fence subject identity");
    }
    fence.subject = *subject;
  }
  fence.fenced_below = CoordinatorEpoch(fenced_below);
  fence.reason = ReasonCode::ScopeFenced;
  if (!reason_text.empty()) {
    static const ReasonCode kKnown[] = {ReasonCode::ScopeFenced, ReasonCode::SubjectFenced,
                                        ReasonCode::RestartEpochAdvanced,
                                        ReasonCode::AttemptFencedByEpochAdvance};
    bool matched = false;
    for (const ReasonCode candidate : kKnown) {
      if (reason_text == ToText(candidate)) {
        fence.reason = candidate;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return Reject("unsupported fence reason");
    }
  }
  status = ReadText(root, "detail", fence.detail);
  if (!status.ok()) return status;
  const Value* created = root.Find("created_unix_ms");
  if (created != nullptr) {
    if (!created->IsNumber()) {
      return Reject("created_unix_ms must be an integer");
    }
    fence.created_unix_ms = created->AsInteger(0);
  }
  out = std::move(fence);
  return Status::Ok();
}

Status ParseJson(std::string_view text, VerificationEvidence& out, const RuntimeLimits& limits) {
  (void)limits;
  json::Value root;
  const Status parsed = json::Parse(text, json::ParseLimits{}, root);
  if (!parsed.ok()) {
    return parsed;
  }
  const Status shape = CheckMembers(root, {"scope", "subject", "idempotency_key", "observation",
                                           "generation", "received_epoch", "verifier", "applier",
                                           "received_unix_ms", "confirms_effect"});
  if (!shape.ok()) {
    return shape;
  }
  std::string scope_text;
  std::string subject_text;
  std::string key_text;
  std::string observation_text;
  std::string verifier_text;
  std::string applier_text;
  std::uint64_t generation = 0;
  std::uint64_t epoch = 0;
  std::int64_t at = 0;
  bool confirms = false;
  Status status = ReadText(root, "scope", scope_text);
  if (!status.ok()) return status;
  status = ReadText(root, "subject", subject_text);
  if (!status.ok()) return status;
  status = ReadText(root, "idempotency_key", key_text);
  if (!status.ok()) return status;
  status = ReadText(root, "observation", observation_text);
  if (!status.ok()) return status;
  status = ReadU64(root, "generation", generation);
  if (!status.ok()) return status;
  status = ReadU64(root, "received_epoch", epoch);
  if (!status.ok()) return status;
  status = ReadText(root, "verifier", verifier_text);
  if (!status.ok()) return status;
  status = ReadText(root, "applier", applier_text);
  if (!status.ok()) return status;
  status = ReadBool(root, "confirms_effect", confirms);
  if (!status.ok()) return status;
  const Value* stamp = root.Find("received_unix_ms");
  if (stamp != nullptr) {
    if (!stamp->IsNumber()) {
      return Reject("received_unix_ms must be an integer");
    }
    at = stamp->AsInteger(0);
  }
  const auto scope = ScopeId::TryParse(scope_text);
  const auto subject = SubjectId::TryParse(subject_text);
  const auto observation = ObservationId::TryParse(observation_text);
  const auto verifier = ReporterId::TryParse(verifier_text);
  const auto applier = ReporterId::TryParse(applier_text);
  if (!scope.has_value() || !subject.has_value() || !observation.has_value() ||
      !verifier.has_value() || !applier.has_value()) {
    return Reject("verification carries an invalid identity");
  }
  VerificationEvidence evidence;
  evidence.scope = *scope;
  evidence.subject = *subject;
  if (!TryParseHexDigest(key_text, evidence.idempotency_key)) {
    return Reject("idempotency_key must be 64 hexadecimal characters");
  }
  evidence.observation = *observation;
  evidence.generation = ObservationGeneration(generation);
  evidence.received_epoch = CoordinatorEpoch(epoch);
  evidence.verifier = *verifier;
  evidence.applier = *applier;
  evidence.received_unix_ms = at;
  evidence.confirms_effect = confirms;
  out = std::move(evidence);
  return Status::Ok();
}

}  // namespace fabric_reconciliation
}  // namespace summon
