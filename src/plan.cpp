// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/plan.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

void EncodePlanContent(CanonicalWriter& writer, const ReconciliationPlan& plan) noexcept {
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(plan.scope.str());
  writer.PutBytes(plan.definition.str());
  writer.PutU64(plan.epoch.value());
  writer.PutDigest(plan.policy_digest);
  writer.PutU64(plan.policy_version.value());
  writer.PutBytes(plan.intent.str());
  writer.PutU64(plan.intent_generation.value());
  writer.PutDigest(plan.intent_digest);
  writer.PutBytes(plan.observation.str());
  writer.PutU64(plan.observation_generation.value());
  writer.PutBytes(plan.reporter.str());
  writer.PutDigest(plan.observation_digest);
  writer.PutU8(static_cast<std::uint8_t>(plan.verdict));
  writer.PutU8(static_cast<std::uint8_t>(plan.evidence));
  writer.PutBool(plan.truncated);
  writer.PutU16(static_cast<std::uint16_t>(plan.scope_reason));
  writer.PutU32(
      static_cast<std::uint32_t>(plan.scope_explanation.steps().size() & 0xffffffffu));
  for (const ExplanationStep& step : plan.scope_explanation.steps()) {
    writer.PutU16(static_cast<std::uint16_t>(step.code));
    writer.PutBytes(step.detail);
  }
  writer.PutU32(static_cast<std::uint32_t>(plan.decisions.size() & 0xffffffffu));
  for (const Decision& decision : plan.decisions) {
    writer.PutBytes(decision.scope.str());
    writer.PutBytes(decision.subject.str());
    writer.PutU8(static_cast<std::uint8_t>(decision.drift));
    writer.PutU16(static_cast<std::uint16_t>(decision.reason));
    writer.PutU8(static_cast<std::uint8_t>(decision.action));
    writer.PutU8(static_cast<std::uint8_t>(decision.disposition));
    writer.PutDigest(decision.idempotency_key);
    writer.PutU8(static_cast<std::uint8_t>(decision.evidence));
    writer.PutU32(static_cast<std::uint32_t>(decision.explanation.steps().size() & 0xffffffffu));
    for (const ExplanationStep& step : decision.explanation.steps()) {
      writer.PutU16(static_cast<std::uint16_t>(step.code));
      writer.PutBytes(step.detail);
    }
  }
}

}  // namespace

const char* ToText(ActionKind value) noexcept {
  switch (value) {
    case ActionKind::None: return "NONE";
    case ActionKind::ApplyDesired: return "APPLY_DESIRED";
    case ActionKind::WithdrawSubject: return "WITHDRAW_SUBJECT";
  }
  return "INVALID";
}

bool TryParseActionKind(const char* text, ActionKind& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::string_view view(text);
  if (view == "NONE") { out = ActionKind::None; return true; }
  if (view == "APPLY_DESIRED") { out = ActionKind::ApplyDesired; return true; }
  if (view == "WITHDRAW_SUBJECT") { out = ActionKind::WithdrawSubject; return true; }
  return false;
}

const char* ToText(DecisionDisposition value) noexcept {
  switch (value) {
    case DecisionDisposition::Invalid: return "INVALID";
    case DecisionDisposition::Actionable: return "ACTIONABLE";
    case DecisionDisposition::Blocked: return "BLOCKED";
    case DecisionDisposition::Conflicted: return "CONFLICTED";
    case DecisionDisposition::Indeterminate: return "INDETERMINATE";
    case DecisionDisposition::AlreadySatisfied: return "ALREADY_SATISFIED";
    case DecisionDisposition::Unsupported: return "UNSUPPORTED";
    case DecisionDisposition::Fenced: return "FENCED";
  }
  return "INVALID";
}

bool IsMutationDisposition(DecisionDisposition value) noexcept {
  return value == DecisionDisposition::Actionable;
}

const char* ToText(ConvergenceVerdict value) noexcept {
  switch (value) {
    case ConvergenceVerdict::Invalid: return "INVALID";
    case ConvergenceVerdict::ConvergedProven: return "CONVERGED_PROVEN";
    case ConvergenceVerdict::ConvergenceRequired: return "CONVERGENCE_REQUIRED";
    case ConvergenceVerdict::ProvenBlocked: return "PROVEN_BLOCKED";
    case ConvergenceVerdict::Indeterminate: return "INDETERMINATE";
    case ConvergenceVerdict::SearchLimitReached: return "SEARCH_LIMIT_REACHED";
  }
  return "INVALID";
}

Sha256Digest ComputeIdempotencyKey(const ScopeId& scope, const SubjectId& subject,
                                   ActionKind action, const AuthorityVector& authority,
                                   const SubjectState& desired,
                                   const SubjectState& expected_observed) noexcept {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(scope.str());
  writer.PutBytes(subject.str());
  writer.PutU8(static_cast<std::uint8_t>(action));
  writer.PutU64(authority.intent_generation.value());
  writer.PutDigest(authority.intent_digest);
  writer.PutU64(authority.observation_generation.value());
  writer.PutDigest(authority.observation_digest);
  writer.PutBytes(authority.definition.str());
  writer.PutDigest(DigestOfState(desired));
  writer.PutDigest(DigestOfState(expected_observed));
  return writer.Digest();
}

std::size_t ReconciliationPlan::MutationCount() const noexcept {
  std::size_t count = 0;
  for (const Decision& decision : decisions) {
    if (IsMutationDisposition(decision.disposition)) {
      ++count;
    }
  }
  return count;
}

std::size_t ReconciliationPlan::CountDisposition(DecisionDisposition value) const noexcept {
  std::size_t count = 0;
  for (const Decision& decision : decisions) {
    if (decision.disposition == value) {
      ++count;
    }
  }
  return count;
}

std::size_t ReconciliationPlan::CountDrift(DriftClass value) const noexcept {
  std::size_t count = 0;
  for (const Decision& decision : decisions) {
    if (decision.drift == value) {
      ++count;
    }
  }
  return count;
}

void ReconciliationPlan::Seal() {
  CanonicalWriter writer;
  EncodePlanContent(writer, *this);
  plan_digest = writer.Digest();
  plan_id = ToHex(plan_digest);
}

}  // namespace fabric_reconciliation
}  // namespace summon
