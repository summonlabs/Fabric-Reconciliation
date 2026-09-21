// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic drift classification.
//
// The classifier is a pure function of (intent, observation, policy, live
// authority, clock reading). It has no other inputs: results do not depend on
// container order, insertion order, hash seeds or the order in which documents
// happened to arrive.

#ifndef SUMMON_FABRIC_RECONCILIATION_DRIFT_HPP
#define SUMMON_FABRIC_RECONCILIATION_DRIFT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/document.hpp"
#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/policy.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/value.hpp"

namespace summon {
namespace fabric_reconciliation {

/// The complete drift vocabulary. Every other value is reported explicitly;
/// none of them is folded into success or into ordinary absence.
enum class DriftClass : std::uint8_t {
  Invalid = 0,
  /// Intent and fresh observation agree on every managed attribute.
  AlreadyConverged = 1,
  /// Intent expects the subject to exist; fresh, absence-authoritative
  /// observation says it does not.
  Missing = 2,
  /// Fresh observation reports a subject the complete intent does not declare.
  Unexpected = 3,
  /// Both exist and at least one managed attribute differs.
  Mismatched = 4,
  /// The best observation for the scope is not fresh. The runtime does not
  /// fall back to an older generation.
  StaleObservation = 5,
  /// The intent is behind the committed intent lineage for the scope, or the
  /// scope has no intent at all where policy requires one.
  StaleIntent = 6,
  /// Two publishers disagree at the same generation, or committed lineage
  /// contradicts the supplied evidence.
  Conflict = 7,
  /// Coverage is insufficient to decide. Explicitly not "already satisfied".
  Unknown = 8,
  /// The subject or attribute lies outside the supported comparison class.
  Unsupported = 9,
};

[[nodiscard]] FR_API const char* ToText(DriftClass value) noexcept;
[[nodiscard]] FR_API bool TryParseDriftClass(const char* text, DriftClass& out) noexcept;

/// True when the class means "this difference requires a reconciliation
/// action and the runtime knows what that action is".
[[nodiscard]] FR_API bool IsActionableDrift(DriftClass value) noexcept;
/// True when the class means "the runtime cannot decide from this evidence".
[[nodiscard]] FR_API bool IsIndeterminateDrift(DriftClass value) noexcept;

/// Why one attribute differs.
enum class AttributeDeltaKind : std::uint8_t {
  Invalid = 0,
  ValueMismatch = 1,
  OnlyInIntent = 2,
  OnlyInObservation = 3,
  NotComparable = 4,
  /// The attribute is excluded from reconciliation by policy.
  Unmanaged = 5,
  /// The attribute is deliberately observe-only by policy.
  ObserveOnly = 6,
};

[[nodiscard]] FR_API const char* ToText(AttributeDeltaKind value) noexcept;

struct FR_API AttributeDelta {
  AttributeKey key;
  AttributeDeltaKind kind{AttributeDeltaKind::Invalid};
  AttributeValue intended;
  AttributeValue observed;

  friend bool operator==(const AttributeDelta&, const AttributeDelta&) = default;
};

/// One classified subject.
struct FR_API DriftRecord {
  ScopeId scope;
  SubjectId subject;
  DriftClass drift{DriftClass::Invalid};
  ReasonCode reason{ReasonCode::None};
  std::vector<AttributeDelta> deltas;
  EvidenceClass evidence{EvidenceClass::Unknown};

  friend bool operator==(const DriftRecord&, const DriftRecord&) = default;
};

/// Outcome of evaluating an observation against the freshness policy.
enum class FreshnessVerdict : std::uint8_t {
  Invalid = 0,
  Fresh = 1,
  /// Older than the temporal horizon.
  Expired = 2,
  /// Recorded by a previous runtime incarnation while policy requires the
  /// current one.
  ForeignEpoch = 3,
  /// The host wall clock moved backwards relative to the record.
  ClockRegression = 4,
};

[[nodiscard]] FR_API const char* ToText(FreshnessVerdict value) noexcept;
[[nodiscard]] FR_API ReasonCode ReasonOf(FreshnessVerdict value) noexcept;

/// Evaluates freshness. Fail-closed: clock regression and epoch mismatch are
/// stale, never fresh.
[[nodiscard]] FR_API FreshnessVerdict EvaluateFreshness(const ObservationDocument& observation,
                                                        const FreshnessPolicy& policy,
                                                        CoordinatorEpoch live_epoch,
                                                        UnixMillis now_unix_ms) noexcept;

/// Evidence references bound into one classification.
struct FR_API ScopeEvidenceRef {
  bool has_intent{false};
  IntentId intent;
  DefinitionId definition;
  IntentGeneration intent_generation;
  Sha256Digest intent_digest{};
  bool intent_complete{true};

  bool has_observation{false};
  ObservationId observation;
  ReporterId reporter;
  ObservationGeneration observation_generation;
  Sha256Digest observation_digest{};
  CoordinatorEpoch received_epoch;
  UnixMillis received_unix_ms{0};
  bool observation_complete{true};
  bool observation_authoritative_absence{false};

  FreshnessVerdict freshness{FreshnessVerdict::Invalid};
  EvidenceClass evidence{EvidenceClass::Unknown};
};

/// The classifier output for one scope.
struct FR_API ClassificationReport {
  ScopeId scope;
  ScopeEvidenceRef evidence;
  std::vector<DriftRecord> records;

  /// Work counters, exposed so that scale proofs can show linear behaviour
  /// rather than infer it from wall-clock time alone.
  std::uint64_t subject_visits{0};
  std::uint64_t attribute_comparisons{0};

  [[nodiscard]] std::size_t CountClass(DriftClass value) const noexcept;
};

/// Selection of the intent a decision may rest on.
///
/// Rule: take the maximum generation. Among candidates at that generation the
/// content must agree; divergent content at one generation is a Conflict and no
/// intent is selected. Selection is a pure function of the candidate set and
/// does not depend on the order the candidates are supplied in.
struct FR_API IntentSelection {
  const IntentDocument* selected{nullptr};
  bool conflicted{false};
  ReasonCode reason{ReasonCode::NoIntentForScope};
};

/// Selection of the observation a decision may rest on.
///
/// Ordering key is (generation, received_unix_ms, reporter text), compared
/// lexicographically. Conflict rule: every retained observation that shares the
/// selected generation must carry the same digest, whatever its reporter. A
/// disagreement at one generation is a Conflict and no observation is selected.
/// The runtime never falls back to an older generation to manufacture
/// freshness.
struct FR_API ObservationSelection {
  const ObservationDocument* selected{nullptr};
  bool conflicted{false};
  ReasonCode reason{ReasonCode::NoObservationForScope};
};

[[nodiscard]] FR_API IntentSelection SelectCurrentIntent(
    const std::vector<const IntentDocument*>& candidates) noexcept;
[[nodiscard]] FR_API ObservationSelection SelectCurrentObservation(
    const std::vector<const ObservationDocument*>& candidates) noexcept;

/// Classification input: exactly the evidence the decision may rest on.
struct FR_API ClassificationInput {
  ScopeId scope;
  /// Candidate intents for the scope. May be null when the runtime holds none.
  const std::vector<const IntentDocument*>* intents{nullptr};
  /// Candidate observations for the scope. May be null.
  const std::vector<const ObservationDocument*>* observations{nullptr};
  /// Committed intent generation for the scope, used to detect regression.
  IntentGeneration committed_intent_generation;
  bool has_committed_intent{false};
  const ReconciliationPolicy* policy{nullptr};
  const RuntimeLimits* limits{nullptr};
  CoordinatorEpoch live_epoch;
  UnixMillis now_unix_ms{0};
};

/// Runs the classifier. Deterministic and free of side effects.
[[nodiscard]] FR_API Status ClassifyScope(const ClassificationInput& input,
                                          ClassificationReport& out);

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_DRIFT_HPP
