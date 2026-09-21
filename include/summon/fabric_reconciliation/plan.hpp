// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciliation decisions, action intents and plans.
//
// A plan is a deterministic, canonically ordered value. Two runs over the same
// evidence produce byte-identical plans, including identifiers. A plan never
// contains an action for a subject that is already converged.

#ifndef SUMMON_FABRIC_RECONCILIATION_PLAN_HPP
#define SUMMON_FABRIC_RECONCILIATION_PLAN_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/authority.hpp"
#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/drift.hpp"
#include "summon/fabric_reconciliation/explain.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/value.hpp"

namespace summon {
namespace fabric_reconciliation {

/// What the runtime would ask a target to do. The runtime never executes these
/// itself: it owns reconciliation *intent* and the legality of issuing it.
enum class ActionKind : std::uint8_t {
  None = 0,
  /// Write the desired attribute set for a subject that should exist.
  ApplyDesired = 1,
  /// Remove a subject that a complete intent does not declare.
  WithdrawSubject = 2,
};

[[nodiscard]] FR_API const char* ToText(ActionKind value) noexcept;
[[nodiscard]] FR_API bool TryParseActionKind(const char* text, ActionKind& out) noexcept;

/// What the runtime decided about one subject.
enum class DecisionDisposition : std::uint8_t {
  Invalid = 0,
  /// A legal, authorized action exists and would be issued.
  Actionable = 1,
  /// A difference exists but policy or coverage forbids acting on it.
  Blocked = 2,
  /// Publishers disagree; the runtime refuses to choose.
  Conflicted = 3,
  /// Evidence is insufficient to decide. Not a difference, not a success.
  Indeterminate = 4,
  /// Nothing to do: intent and fresh observation already agree.
  AlreadySatisfied = 5,
  /// Outside the supported comparison or action class.
  Unsupported = 6,
  /// Authority was revoked by a fence or an epoch advance.
  Fenced = 7,
};

[[nodiscard]] FR_API const char* ToText(DecisionDisposition value) noexcept;
[[nodiscard]] FR_API bool IsMutationDisposition(DecisionDisposition value) noexcept;

/// One decision, complete with the authority that made it legal.
struct FR_API Decision {
  ScopeId scope;
  SubjectId subject;
  DriftClass drift{DriftClass::Invalid};
  ReasonCode reason{ReasonCode::None};
  ActionKind action{ActionKind::None};
  DecisionDisposition disposition{DecisionDisposition::Invalid};
  AuthorityVector authority;
  /// Deterministic idempotency key. Equal keys mean "same effect, issued for
  /// the same reason against the same evidence".
  Sha256Digest idempotency_key{};
  Explanation explanation;
  EvidenceClass evidence{EvidenceClass::Unknown};

  friend bool operator==(const Decision&, const Decision&) = default;
};

/// Outcome vocabulary for the whole plan.
enum class ConvergenceVerdict : std::uint8_t {
  Invalid = 0,
  /// Every subject in scope was classified with complete coverage and no
  /// difference remains. This is a proof, and it rests on complete coverage.
  ConvergedProven = 1,
  /// Legal, authorized actions exist that would move the fabric towards the
  /// intent.
  ConvergenceRequired = 2,
  /// No legal plan can reach the intent under the current policy and
  /// authority. The certificate is the complete list of blocking decisions.
  ProvenBlocked = 3,
  /// The runtime cannot decide: coverage gaps, staleness, conflicts or
  /// unsupported subjects are present. Explicitly not success.
  Indeterminate = 4,
  /// A configured bound stopped classification. Proof strength is reduced and
  /// the caller must not treat this as completeness.
  SearchLimitReached = 5,
};

[[nodiscard]] FR_API const char* ToText(ConvergenceVerdict value) noexcept;

/// A complete, deterministic reconciliation plan.
struct FR_API ReconciliationPlan {
  ScopeId scope;
  DefinitionId definition;
  CoordinatorEpoch epoch;
  BootId boot;
  PolicyId policy_id;
  PolicyVersion policy_version;
  Sha256Digest policy_digest{};
  IntentGeneration intent_generation;
  IntentId intent;
  Sha256Digest intent_digest{};
  ObservationId observation;
  ObservationGeneration observation_generation;
  ReporterId reporter;
  Sha256Digest observation_digest{};
  CoordinatorEpoch observation_epoch;
  UnixMillis observation_received_unix_ms{0};
  FreshnessVerdict freshness{FreshnessVerdict::Invalid};
  ConvergenceVerdict verdict{ConvergenceVerdict::Invalid};
  EvidenceClass evidence{EvidenceClass::Unknown};

  /// True when a bound stopped the runtime before every subject was decided.
  bool truncated{false};
  std::vector<Decision> decisions;

  /// Scope-level explanation of the verdict itself: why the plan is a proof,
  /// why it is indeterminate, or which bound stopped the runtime.
  Explanation scope_explanation;
  ReasonCode scope_reason{ReasonCode::None};

  /// Canonical digest over the whole plan content, and its hex rendering.
  Sha256Digest plan_digest{};
  std::string plan_id;

  /// Number of decisions that would issue a mutation.
  [[nodiscard]] std::size_t MutationCount() const noexcept;
  [[nodiscard]] std::size_t CountDisposition(DecisionDisposition value) const noexcept;
  [[nodiscard]] std::size_t CountDrift(DriftClass value) const noexcept;

  /// Recomputes and stores plan_digest and plan_id.
  void Seal();
};

/// Deterministic idempotency key for one action.
///
/// The key binds the scope, the subject, the action, the intent generation and
/// digest, the observation generation and digest, and the exact expected
/// observed state the action is conditioned on. Any change to any of those
/// produces a different key, so a stale in-flight action can never be mistaken
/// for a current one.
[[nodiscard]] FR_API Sha256Digest ComputeIdempotencyKey(const ScopeId& scope,
                                                        const SubjectId& subject,
                                                        ActionKind action,
                                                        const AuthorityVector& authority,
                                                        const SubjectState& desired,
                                                        const SubjectState& expected_observed) noexcept;

/// Input to planning.
struct FR_API PlanRequest {
  ScopeId scope;
  /// Policy to plan under. When null, the runtime uses the active policy.
  const ReconciliationPolicy* policy{nullptr};
  /// Clock reading used for freshness. Supplied so that proofs are
  /// reproducible; the runtime uses its own clock when zero.
  UnixMillis now_unix_ms{0};
  /// When true the runtime performs no durable mutation of any kind.
  bool dry_run{false};
};

/// A dispatched, durable action intent.
struct FR_API ActionIntent {
  ScopeId scope;
  SubjectId subject;
  ActionKind action{ActionKind::None};
  SubjectState desired;
  SubjectState expected_observed;
  AuthorityVector authority;
  Sha256Digest idempotency_key{};
  AttemptId attempt;
  CoordinatorEpoch epoch;
  BootId boot;
  UnixMillis issued_unix_ms{0};

  friend bool operator==(const ActionIntent&, const ActionIntent&) = default;
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_PLAN_HPP
