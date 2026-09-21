// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/drift.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace summon {
namespace fabric_reconciliation {
namespace {

bool ObservationKeyLess(const ObservationDocument& lhs, const ObservationDocument& rhs) noexcept {
  if (!(lhs.generation == rhs.generation)) {
    return lhs.generation < rhs.generation;
  }
  if (lhs.received_unix_ms != rhs.received_unix_ms) {
    return lhs.received_unix_ms < rhs.received_unix_ms;
  }
  return lhs.reporter < rhs.reporter;
}

template <typename Map>
void CollectSubjects(const Map& subjects, std::set<SubjectId>& into) {
  for (const auto& entry : subjects) {
    into.insert(entry.first);
  }
}

DriftRecord MakeRecord(const ScopeId& scope, const SubjectId& subject, DriftClass drift,
                       ReasonCode reason, EvidenceClass evidence) {
  DriftRecord record;
  record.scope = scope;
  record.subject = subject;
  record.drift = drift;
  record.reason = reason;
  record.evidence = evidence;
  return record;
}

/// Walks the merged attribute key sets of one subject and reports every
/// difference. Coverage gaps are reported separately from proven differences:
/// a gap never becomes an absence.
struct AttributeWalk {
  std::vector<AttributeDelta> deltas;
  bool coverage_gap{false};
  bool unsupported{false};
  std::uint64_t comparisons{0};
};

AttributeWalk WalkAttributes(const SubjectState& desired, const SubjectState& observed,
                             bool intent_complete, bool observation_complete,
                             const ReconciliationPolicy& policy) {
  AttributeWalk walk;
  auto left = desired.begin();
  auto right = observed.begin();
  while (left != desired.end() || right != observed.end()) {
    if (right == observed.end() || (left != desired.end() && left->first < right->first)) {
      ++walk.comparisons;
      if (IsAttributeUnmanaged(policy, left->first)) {
        // Reported, never actionable: the reader can see exactly which
        // differences policy excluded from reconciliation.
        AttributeDelta delta;
        delta.key = left->first;
        delta.kind = AttributeDeltaKind::Unmanaged;
        delta.intended = left->second;
        delta.observed = AttributeValue::Absent();
        walk.deltas.push_back(std::move(delta));
      } else if (observation_complete) {
        AttributeDelta delta;
        delta.key = left->first;
        delta.kind = AttributeDeltaKind::OnlyInIntent;
        delta.intended = left->second;
        delta.observed = AttributeValue::Absent();
        walk.deltas.push_back(std::move(delta));
      } else {
        walk.coverage_gap = true;
      }
      ++left;
      continue;
    }
    if (left == desired.end() || right->first < left->first) {
      ++walk.comparisons;
      if (IsAttributeUnmanaged(policy, right->first)) {
        AttributeDelta delta;
        delta.key = right->first;
        delta.kind = AttributeDeltaKind::Unmanaged;
        delta.intended = AttributeValue::Absent();
        delta.observed = right->second;
        walk.deltas.push_back(std::move(delta));
      } else if (intent_complete) {
        AttributeDelta delta;
        delta.key = right->first;
        delta.kind = AttributeDeltaKind::OnlyInObservation;
        delta.intended = AttributeValue::Absent();
        delta.observed = right->second;
        walk.deltas.push_back(std::move(delta));
      } else {
        walk.coverage_gap = true;
      }
      ++right;
      continue;
    }
    ++walk.comparisons;
    if (IsAttributeUnmanaged(policy, left->first)) {
      AttributeDelta delta;
      delta.key = left->first;
      delta.kind = AttributeDeltaKind::Unmanaged;
      delta.intended = left->second;
      delta.observed = right->second;
      walk.deltas.push_back(std::move(delta));
    } else {
      if (!left->second.IsValid() || !right->second.IsValid()) {
        walk.unsupported = true;
      } else if (!left->second.ComparableWith(right->second)) {
        AttributeDelta delta;
        delta.key = left->first;
        delta.kind = AttributeDeltaKind::NotComparable;
        delta.intended = left->second;
        delta.observed = right->second;
        walk.deltas.push_back(std::move(delta));
      } else if (!(left->second == right->second)) {
        AttributeDelta delta;
        delta.key = left->first;
        delta.kind = IsAttributeObserveOnly(policy, left->first) ? AttributeDeltaKind::ObserveOnly
                                                                 : AttributeDeltaKind::ValueMismatch;
        delta.intended = left->second;
        delta.observed = right->second;
        walk.deltas.push_back(std::move(delta));
      }
    }
    ++left;
    ++right;
  }
  return walk;
}

bool HasActionableDelta(const std::vector<AttributeDelta>& deltas) noexcept {
  for (const AttributeDelta& delta : deltas) {
    if (delta.kind != AttributeDeltaKind::Unmanaged) {
      return true;
    }
  }
  return false;
}

ReasonCode MismatchReason(const std::vector<AttributeDelta>& deltas) noexcept {
  ReasonCode reason = ReasonCode::ClassifiedAttributeMismatch;
  int best = 99;
  for (const AttributeDelta& delta : deltas) {
    int rank = 99;
    switch (delta.kind) {
      case AttributeDeltaKind::NotComparable: rank = 0; break;
      case AttributeDeltaKind::ValueMismatch: rank = 1; break;
      case AttributeDeltaKind::ObserveOnly: rank = 2; break;
      case AttributeDeltaKind::OnlyInIntent: rank = 3; break;
      case AttributeDeltaKind::OnlyInObservation: rank = 4; break;
      case AttributeDeltaKind::Unmanaged: continue;
      case AttributeDeltaKind::Invalid: continue;
    }
    if (rank < best) {
      best = rank;
      switch (delta.kind) {
        case AttributeDeltaKind::NotComparable:
          reason = ReasonCode::ClassifiedAttributeNotComparable;
          break;
        case AttributeDeltaKind::ValueMismatch:
        case AttributeDeltaKind::ObserveOnly:
          reason = ReasonCode::ClassifiedAttributeMismatch;
          break;
        case AttributeDeltaKind::OnlyInIntent:
          reason = ReasonCode::ClassifiedAttributeMissing;
          break;
        case AttributeDeltaKind::OnlyInObservation:
          reason = ReasonCode::ClassifiedAttributeUnexpected;
          break;
        default:
          break;
      }
    }
  }
  return reason;
}

}  // namespace

const char* ToText(DriftClass value) noexcept {
  switch (value) {
    case DriftClass::Invalid: return "INVALID";
    case DriftClass::AlreadyConverged: return "ALREADY_CONVERGED";
    case DriftClass::Missing: return "MISSING";
    case DriftClass::Unexpected: return "UNEXPECTED";
    case DriftClass::Mismatched: return "MISMATCHED";
    case DriftClass::StaleObservation: return "STALE_OBSERVATION";
    case DriftClass::StaleIntent: return "STALE_INTENT";
    case DriftClass::Conflict: return "CONFLICT";
    case DriftClass::Unknown: return "UNKNOWN";
    case DriftClass::Unsupported: return "UNSUPPORTED";
  }
  return "INVALID";
}

bool TryParseDriftClass(const char* text, DriftClass& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::string_view view(text);
  if (view == "ALREADY_CONVERGED") { out = DriftClass::AlreadyConverged; return true; }
  if (view == "MISSING") { out = DriftClass::Missing; return true; }
  if (view == "UNEXPECTED") { out = DriftClass::Unexpected; return true; }
  if (view == "MISMATCHED") { out = DriftClass::Mismatched; return true; }
  if (view == "STALE_OBSERVATION") { out = DriftClass::StaleObservation; return true; }
  if (view == "STALE_INTENT") { out = DriftClass::StaleIntent; return true; }
  if (view == "CONFLICT") { out = DriftClass::Conflict; return true; }
  if (view == "UNKNOWN") { out = DriftClass::Unknown; return true; }
  if (view == "UNSUPPORTED") { out = DriftClass::Unsupported; return true; }
  return false;
}

bool IsActionableDrift(DriftClass value) noexcept {
  return value == DriftClass::Missing || value == DriftClass::Unexpected ||
         value == DriftClass::Mismatched;
}

bool IsIndeterminateDrift(DriftClass value) noexcept {
  return value == DriftClass::Unknown || value == DriftClass::StaleObservation ||
         value == DriftClass::StaleIntent || value == DriftClass::Invalid;
}

const char* ToText(AttributeDeltaKind value) noexcept {
  switch (value) {
    case AttributeDeltaKind::Invalid: return "INVALID";
    case AttributeDeltaKind::ValueMismatch: return "VALUE_MISMATCH";
    case AttributeDeltaKind::OnlyInIntent: return "ONLY_IN_INTENT";
    case AttributeDeltaKind::OnlyInObservation: return "ONLY_IN_OBSERVATION";
    case AttributeDeltaKind::NotComparable: return "NOT_COMPARABLE";
    case AttributeDeltaKind::Unmanaged: return "UNMANAGED";
    case AttributeDeltaKind::ObserveOnly: return "OBSERVE_ONLY";
  }
  return "INVALID";
}

const char* ToText(FreshnessVerdict value) noexcept {
  switch (value) {
    case FreshnessVerdict::Invalid: return "INVALID";
    case FreshnessVerdict::Fresh: return "FRESH";
    case FreshnessVerdict::Expired: return "EXPIRED";
    case FreshnessVerdict::ForeignEpoch: return "FOREIGN_EPOCH";
    case FreshnessVerdict::ClockRegression: return "CLOCK_REGRESSION";
  }
  return "INVALID";
}

ReasonCode ReasonOf(FreshnessVerdict value) noexcept {
  switch (value) {
    case FreshnessVerdict::Fresh: return ReasonCode::None;
    case FreshnessVerdict::Expired: return ReasonCode::ObservationExpired;
    case FreshnessVerdict::ForeignEpoch: return ReasonCode::ObservationForeignEpoch;
    case FreshnessVerdict::ClockRegression: return ReasonCode::ObservationClockRegression;
    case FreshnessVerdict::Invalid: return ReasonCode::NoObservationForScope;
  }
  return ReasonCode::NoObservationForScope;
}

FreshnessVerdict EvaluateFreshness(const ObservationDocument& observation,
                                   const FreshnessPolicy& policy, CoordinatorEpoch live_epoch,
                                   UnixMillis now_unix_ms) noexcept {
  if (policy.require_current_epoch && !(observation.received_epoch == live_epoch)) {
    return FreshnessVerdict::ForeignEpoch;
  }
  if (policy.max_observation_age_ms > 0) {
    if (now_unix_ms < observation.received_unix_ms) {
      return FreshnessVerdict::ClockRegression;
    }
    if (now_unix_ms - observation.received_unix_ms > policy.max_observation_age_ms) {
      return FreshnessVerdict::Expired;
    }
  }
  return FreshnessVerdict::Fresh;
}

IntentSelection SelectCurrentIntent(
    const std::vector<const IntentDocument*>& candidates) noexcept {
  IntentSelection selection;
  for (const IntentDocument* document : candidates) {
    if (document == nullptr) {
      continue;
    }
    if (selection.selected == nullptr || selection.selected->generation < document->generation) {
      selection.selected = document;
    }
  }
  if (selection.selected == nullptr) {
    selection.reason = ReasonCode::NoIntentForScope;
    return selection;
  }
  for (const IntentDocument* document : candidates) {
    if (document == nullptr) {
      continue;
    }
    if (document->generation == selection.selected->generation &&
        !(document->digest == selection.selected->digest)) {
      selection.selected = nullptr;
      selection.conflicted = true;
      selection.reason = ReasonCode::ConflictingRepublishers;
      return selection;
    }
  }
  selection.reason = ReasonCode::None;
  return selection;
}

ObservationSelection SelectCurrentObservation(
    const std::vector<const ObservationDocument*>& candidates) noexcept {
  ObservationSelection selection;
  for (const ObservationDocument* document : candidates) {
    if (document == nullptr) {
      continue;
    }
    if (selection.selected == nullptr || ObservationKeyLess(*selection.selected, *document)) {
      selection.selected = document;
    }
  }
  if (selection.selected == nullptr) {
    selection.reason = ReasonCode::NoObservationForScope;
    return selection;
  }
  for (const ObservationDocument* document : candidates) {
    if (document == nullptr) {
      continue;
    }
    if (document->generation == selection.selected->generation &&
        !(document->digest == selection.selected->digest)) {
      selection.selected = nullptr;
      selection.conflicted = true;
      selection.reason = ReasonCode::ConflictingRepublishers;
      return selection;
    }
  }
  selection.reason = ReasonCode::None;
  return selection;
}

std::size_t ClassificationReport::CountClass(DriftClass value) const noexcept {
  std::size_t count = 0;
  for (const DriftRecord& record : records) {
    if (record.drift == value) {
      ++count;
    }
  }
  return count;
}

Status ClassifyScope(const ClassificationInput& input, ClassificationReport& out) {
  if (input.policy == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DriftIsIndeterminate,
                  "classification requires an explicit policy");
  }
  if (input.scope.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "classification requires a non-empty scope");
  }
  const RuntimeLimits limits = (input.limits != nullptr) ? *input.limits : RuntimeLimits{};
  const ReconciliationPolicy& policy = *input.policy;

  std::vector<const IntentDocument*> intents;
  if (input.intents != nullptr) {
    intents = *input.intents;
  }
  std::vector<const ObservationDocument*> observations;
  if (input.observations != nullptr) {
    observations = *input.observations;
  }
  for (const IntentDocument* document : intents) {
    if (document != nullptr && !(document->scope == input.scope)) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "candidate intent belongs to a different scope");
    }
  }
  for (const ObservationDocument* document : observations) {
    if (document != nullptr && !(document->scope == input.scope)) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "candidate observation belongs to a different scope");
    }
  }

  ClassificationReport report;
  report.scope = input.scope;
  ScopeEvidenceRef& evidence = report.evidence;

  const IntentSelection intent_selection = SelectCurrentIntent(intents);
  const ObservationSelection observation_selection = SelectCurrentObservation(observations);

  if (intent_selection.selected != nullptr) {
    const IntentDocument& document = *intent_selection.selected;
    evidence.has_intent = true;
    evidence.intent = document.intent;
    evidence.definition = document.definition;
    evidence.intent_generation = document.generation;
    evidence.intent_digest = document.digest;
    evidence.intent_complete = document.complete;
  }
  if (observation_selection.selected != nullptr) {
    const ObservationDocument& document = *observation_selection.selected;
    evidence.has_observation = true;
    evidence.observation = document.observation;
    evidence.reporter = document.reporter;
    evidence.observation_generation = document.generation;
    evidence.observation_digest = document.digest;
    evidence.received_epoch = document.received_epoch;
    evidence.received_unix_ms = document.received_unix_ms;
    evidence.observation_complete = document.complete;
    evidence.observation_authoritative_absence = document.authoritative_absence;
    evidence.freshness =
        EvaluateFreshness(document, policy.freshness, input.live_epoch, input.now_unix_ms);
  }

  const bool intent_regressed =
      input.has_committed_intent && intent_selection.selected != nullptr &&
      intent_selection.selected->generation < input.committed_intent_generation;

  EvidenceClass evidence_class = EvidenceClass::Unknown;
  if (intent_selection.selected != nullptr) {
    evidence_class = intent_selection.selected->evidence;
  }
  if (observation_selection.selected != nullptr &&
      observation_selection.selected->evidence != EvidenceClass::Unknown) {
    evidence_class = observation_selection.selected->evidence;
  }
  evidence.evidence = evidence_class;

  std::set<SubjectId> subjects;
  for (const IntentDocument* document : intents) {
    if (document != nullptr) {
      CollectSubjects(document->subjects, subjects);
    }
  }
  for (const ObservationDocument* document : observations) {
    if (document != nullptr) {
      CollectSubjects(document->subjects, subjects);
    }
  }

  const bool definite_conflict =
      intent_selection.conflicted || observation_selection.conflicted;
  const bool definite_stale_intent =
      !definite_conflict && (intent_regressed || (!evidence.has_intent && input.has_committed_intent));
  const bool missing_intent =
      !definite_conflict && !definite_stale_intent && !evidence.has_intent;
  const bool stale_observation =
      !definite_conflict && !definite_stale_intent && !missing_intent &&
      evidence.has_observation && evidence.freshness != FreshnessVerdict::Fresh;

  if (definite_conflict) {
    const ReasonCode reason = intent_selection.conflicted ? intent_selection.reason
                                                         : observation_selection.reason;
    for (const SubjectId& subject : subjects) {
      report.records.push_back(
          MakeRecord(input.scope, subject, DriftClass::Conflict, reason, evidence_class));
    }
    out = std::move(report);
    return Status::Ok();
  }

  if (definite_stale_intent) {
    const ReasonCode reason = intent_regressed ? ReasonCode::IntentGenerationRegressed
                                               : ReasonCode::NoIntentForScope;
    for (const SubjectId& subject : subjects) {
      report.records.push_back(
          MakeRecord(input.scope, subject, DriftClass::StaleIntent, reason, evidence_class));
    }
    out = std::move(report);
    return Status::Ok();
  }

  if (missing_intent) {
    for (const SubjectId& subject : subjects) {
      report.records.push_back(MakeRecord(input.scope, subject, DriftClass::Unknown,
                                          ReasonCode::NoIntentForScope, evidence_class));
    }
    out = std::move(report);
    return Status::Ok();
  }

  if (intent_selection.selected == nullptr) {
    // Unreachable: every path that leaves the selection empty returned above.
    // The check is explicit so that the invariant is enforced rather than
    // assumed, and so that static analysis can see it.
    return Status(StatusCode::InternalError, ReasonCode::DriftIsIndeterminate,
                  "intent selection is empty after every empty-selection path returned");
  }
  const IntentDocument& intent = *intent_selection.selected;

  if (!evidence.has_observation) {
    for (const auto& entry : intent.subjects) {
      report.records.push_back(MakeRecord(input.scope, entry.first, DriftClass::Unknown,
                                          ReasonCode::NoObservationForScope, evidence_class));
    }
    out = std::move(report);
    return Status::Ok();
  }

  if (stale_observation) {
    const ReasonCode reason = ReasonOf(evidence.freshness);
    for (const SubjectId& subject : subjects) {
      report.records.push_back(
          MakeRecord(input.scope, subject, DriftClass::StaleObservation, reason, evidence_class));
    }
    out = std::move(report);
    return Status::Ok();
  }

  if (observation_selection.selected == nullptr) {
    // Unreachable for the same reason as above.
    return Status(StatusCode::InternalError, ReasonCode::DriftIsIndeterminate,
                  "observation selection is empty after the freshness gate");
  }
  const ObservationDocument& observation = *observation_selection.selected;
  const bool absence_allowed =
      !policy.freshness.require_complete_coverage_for_absence ||
      (observation.complete && observation.authoritative_absence);
  const bool unexpected_allowed = intent.complete;

  auto left = intent.subjects.begin();
  auto right = observation.subjects.begin();
  while (left != intent.subjects.end() || right != observation.subjects.end()) {
    ++report.subject_visits;
    if (right == observation.subjects.end() ||
        (left != intent.subjects.end() && left->first < right->first)) {
      const SubjectIntent& declared = left->second;
      if (absence_allowed) {
        DriftRecord record = MakeRecord(input.scope, left->first, DriftClass::Missing,
                                        ReasonCode::ClassifiedSubjectMissing, evidence_class);
        for (const auto& attribute : declared.desired) {
          if (IsAttributeUnmanaged(policy, attribute.first)) {
            continue;
          }
          AttributeDelta delta;
          delta.key = attribute.first;
          delta.kind = AttributeDeltaKind::OnlyInIntent;
          delta.intended = attribute.second;
          delta.observed = AttributeValue::Absent();
          record.deltas.push_back(std::move(delta));
        }
        report.records.push_back(std::move(record));
      } else {
        const ReasonCode reason = !observation.complete
                                      ? ReasonCode::ObservationCoverageIncomplete
                                      : ReasonCode::ObservationCannotAssertAbsence;
        report.records.push_back(MakeRecord(input.scope, left->first, DriftClass::Unknown, reason,
                                            evidence_class));
      }
      ++left;
      continue;
    }
    if (left == intent.subjects.end() || right->first < left->first) {
      if (unexpected_allowed) {
        DriftRecord record = MakeRecord(input.scope, right->first, DriftClass::Unexpected,
                                        ReasonCode::ClassifiedSubjectUnexpected, evidence_class);
        for (const auto& attribute : right->second.observed) {
          if (IsAttributeUnmanaged(policy, attribute.first)) {
            continue;
          }
          AttributeDelta delta;
          delta.key = attribute.first;
          delta.kind = AttributeDeltaKind::OnlyInObservation;
          delta.intended = AttributeValue::Absent();
          delta.observed = attribute.second;
          record.deltas.push_back(std::move(delta));
        }
        report.records.push_back(std::move(record));
      } else {
        report.records.push_back(MakeRecord(input.scope, right->first, DriftClass::Unknown,
                                            ReasonCode::IntentCoverageIncomplete, evidence_class));
      }
      ++right;
      continue;
    }

    const SubjectIntent& declared = left->second;
    const SubjectObservation& seen = right->second;
    AttributeWalk walk = WalkAttributes(declared.desired, seen.observed,
                                        declared.complete_attributes, seen.complete_attributes,
                                        policy);
    report.attribute_comparisons += walk.comparisons;

    if (walk.unsupported) {
      DriftRecord record = MakeRecord(input.scope, left->first, DriftClass::Unsupported,
                                      ReasonCode::SubjectKindUnsupported, evidence_class);
      record.deltas = std::move(walk.deltas);
      report.records.push_back(std::move(record));
    } else if (walk.coverage_gap) {
      DriftRecord record = MakeRecord(input.scope, left->first, DriftClass::Unknown,
                                      ReasonCode::AttributeCoverageIncomplete, evidence_class);
      record.deltas = std::move(walk.deltas);
      report.records.push_back(std::move(record));
    } else if (!HasActionableDelta(walk.deltas)) {
      DriftRecord record = MakeRecord(input.scope, left->first, DriftClass::AlreadyConverged,
                                      ReasonCode::ClassifiedAlreadyConverged, evidence_class);
      record.deltas = std::move(walk.deltas);
      report.records.push_back(std::move(record));
    } else {
      DriftRecord record = MakeRecord(input.scope, left->first, DriftClass::Mismatched,
                                      MismatchReason(walk.deltas), evidence_class);
      record.deltas = std::move(walk.deltas);
      report.records.push_back(std::move(record));
    }
    ++left;
    ++right;
  }

  (void)limits;
  out = std::move(report);
  return Status::Ok();
}

}  // namespace fabric_reconciliation
}  // namespace summon
