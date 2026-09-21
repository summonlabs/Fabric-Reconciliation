// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial proofs: stale and newer generation combinations, duplicate
// resources, missing dependencies, conflicting publishers, partial apply and
// late completions must all be surfaced explicitly and must never be promoted
// into success.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

const char* kScope = "fabric/rack-7";

struct Rig {
  ScratchDirectory scratch{"adversarial"};
  std::unique_ptr<fr::ReconciliationEngine> engine;
  Rig() { engine = OpenEngine(scratch.path()); }

  fr::Status Seed(const fr::IntentDocument& intent, const fr::ObservationSubmission& observation,
                  fr::UnixMillis now = 1000) {
    fr::IntentCommitResult commit_result;
    const fr::Status committed = engine->CommitIntent(intent, commit_result);
    if (!committed.ok()) {
      return committed;
    }
    fr::ObservationCommitResult observation_result;
    return engine->RecordObservationAt(observation, now, observation_result);
  }

  fr::Result<fr::ReconciliationPlan> Plan(fr::UnixMillis now = 1000, bool dry_run = false) {
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = now;
    request.dry_run = dry_run;
    return engine->Plan(request);
  }
};

fr::ReconciliationOutcome CompleteFirst(Rig& rig, const fr::ReconciliationPlan& plan,
                                        fr::AttemptState target) {
  const auto dispatched = rig.engine->Dispatch(plan);
  if (!dispatched.ok() || dispatched->intents.empty()) {
    throw std::runtime_error("dispatch produced no intent");
  }
  fr::CompletionRequest completion;
  completion.attempt = dispatched->intents.front().attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = dispatched->intents.front().idempotency_key;
  completion.target = target;
  completion.at_unix_ms = 1500;
  fr::ReconciliationOutcome outcome;
  const fr::Status status = rig.engine->Complete(completion, outcome);
  if (!status.ok()) {
    throw std::runtime_error("completion failed: " + status.ToString());
  }
  return outcome;
}

}  // namespace

FR_TEST(adversarial, older_intent_never_overwrites_a_newer_observed_generation) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto first = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(first);
  FR_CHECK(first->MutationCount() > 0);

  // A newer observation arrives that reports the fabric already converged.
  const fr::ObservationSubmission converged =
      MakeObservation(kScope, "reporter/a", 2, 4, 0, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(converged, 1000, result));

  // Re-dispatching the old plan must be refused: the observation generation it
  // was built against is no longer the current evidence.
  const auto dispatched = rig.engine->Dispatch(*first);
  FR_CHECK(!dispatched.ok());
  FR_CHECK(dispatched.status().code() == fr::StatusCode::StaleAuthority);

  const auto second = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(second);
  FR_CHECK(second->verdict == fr::ConvergenceVerdict::ConvergedProven);
  FR_CHECK_EQ(second->MutationCount(), std::size_t(0));
  FR_CHECK(second->observation_generation.value() == 2);
}

FR_TEST(adversarial, newer_intent_never_overwrites_a_newer_observed_generation) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 5, 4, 0, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::ConvergedProven);

  // A newer intent asks for something the newest observation does not show.
  fr::IntentDocument newer = MakeIntent(kScope, 2, 4, fr::EvidenceClass::Synthetic);
  for (auto& entry : newer.subjects) {
    entry.second.desired[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(9216);
  }
  newer.digest = fr::ComputeIntentDigest(newer);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(newer, commit_result));

  const auto second = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(second);
  FR_CHECK(second->verdict == fr::ConvergenceVerdict::ConvergenceRequired);
  FR_CHECK_EQ(second->intent_generation.value(), 2ull);
  FR_CHECK(second->observation_generation.value() == 5);
  FR_CHECK(second->MutationCount() == 4);
}

FR_TEST(adversarial, duplicate_resources_are_deduplicated_not_double_actioned) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 2, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const std::size_t expected = plan->MutationCount();

  for (int iteration = 0; iteration < 5; ++iteration) {
    const auto dispatched = rig.engine->Dispatch(*plan);
    FR_CHECK_STATUS_OK(dispatched);
    FR_CHECK_EQ(dispatched->intents.size(), expected);
  }
  const auto attempts = rig.engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), expected);
}

FR_TEST(adversarial, missing_dependency_observation_yields_unknown_not_missing) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(intent, commit_result));

  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK_EQ(plan->reporter.str(), std::string());
  FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::Unknown), std::size_t(4));
  FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::Missing), std::size_t(0));
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
}

FR_TEST(adversarial, conflicting_publishers_never_produce_actions) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(intent, commit_result));

  const fr::ObservationSubmission left =
      MakeObservation(kScope, "reporter/a", 3, 4, 0, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission right =
      MakeObservation(kScope, "reporter/b", 3, 4, 2, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(left, 1000, result));
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(right, 1000, result));

  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::Conflict), std::size_t(4));
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  for (const fr::Decision& decision : plan->decisions) {
    FR_CHECK(decision.disposition == fr::DecisionDisposition::Conflicted);
  }
}

FR_TEST(adversarial, partial_apply_is_detected_and_remains_actionable) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 2, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(intent, commit_result));

  // The first observation reports both subjects mismatched, the second reports
  // one of them converged: a partial apply.
  const fr::ObservationSubmission before =
      MakeObservation(kScope, "reporter/a", 1, 2, 1, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(before, 1000, result));

  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(2));
  FR_CHECK_STATUS_OK(rig.engine->Dispatch(*plan));

  fr::ObservationSubmission after =
      MakeObservation(kScope, "reporter/a", 2, 2, 1, false, fr::EvidenceClass::Synthetic);
  after.subjects.begin()->second.observed[RequireAttribute("mtu")] =
      fr::AttributeValue::Unsigned(9000);
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(after, 1000, result));

  const auto second = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(second);
  FR_CHECK(second->verdict == fr::ConvergenceVerdict::ConvergenceRequired);
  FR_CHECK_EQ(second->MutationCount(), std::size_t(1));
  FR_CHECK(second->CountDrift(fr::DriftClass::AlreadyConverged) == 1);
  FR_CHECK(second->CountDrift(fr::DriftClass::Mismatched) == 1);
}

FR_TEST(adversarial, late_completion_is_fenced_and_changes_nothing) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 2, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  FR_CHECK(!dispatched->intents.empty());

  const fr::ActionIntent& action = dispatched->intents.front();
  fr::CompletionRequest completion;
  completion.attempt = action.attempt;
  completion.epoch = fr::CoordinatorEpoch(rig.engine->epoch().value() - 1);
  completion.boot = rig.engine->boot();
  completion.idempotency_key = action.idempotency_key;
  completion.target = fr::AttemptState::Applied;
  fr::ReconciliationOutcome outcome;
  const fr::Status status = rig.engine->Complete(completion, outcome);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::StaleAuthority);
  FR_CHECK(status.reason() == fr::ReasonCode::AttemptFencedByEpochAdvance);

  const auto attempts = rig.engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  for (const fr::AttemptRecord& attempt : *attempts) {
    FR_CHECK(!(attempt.id == action.attempt) || attempt.state == fr::AttemptState::Dispatched);
  }
  FR_CHECK_EQ(rig.engine->stats().completions_rejected_late, 1ull);
}

FR_TEST(adversarial, completing_an_attempt_does_not_issue_a_second_attempt) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const fr::ReconciliationOutcome outcome = CompleteFirst(rig, *plan, fr::AttemptState::Applied);
  FR_CHECK(outcome.terminal_state == fr::AttemptState::Applied);
  FR_CHECK(!outcome.verified);
  FR_CHECK(!outcome.ambiguous);

  // Completing an attempt leaves the world unchanged: the same plan still
  // reports the same single outstanding action and dispatching it again reuses
  // the same attempt rather than issuing a second one.
  const auto again = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(again);
  FR_CHECK_EQ(again->MutationCount(), std::size_t(1));
  const auto dispatched = rig.engine->Dispatch(*again);
  FR_CHECK_STATUS_OK(dispatched);
  FR_CHECK_EQ(dispatched->intents.size(), std::size_t(1));
  FR_CHECK_EQ(dispatched->already_issued, std::size_t(1));
  FR_CHECK(dispatched->intents[0].attempt.value() == outcome.attempt.value());
}

FR_TEST(adversarial, duplicate_completion_after_terminal_state_is_refused) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent& action = dispatched->intents.front();

  fr::CompletionRequest completion;
  completion.attempt = action.attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = action.idempotency_key;
  completion.target = fr::AttemptState::Failed;
  completion.at_unix_ms = 1600;
  fr::ReconciliationOutcome outcome;
  FR_CHECK_STATUS_OK(rig.engine->Complete(completion, outcome));
  FR_CHECK(outcome.terminal_state == fr::AttemptState::Failed);
  FR_CHECK(outcome.ambiguous);

  completion.target = fr::AttemptState::Applied;
  const fr::Status again = rig.engine->Complete(completion, outcome);
  FR_CHECK(!again.ok());
  FR_CHECK(again.code() == fr::StatusCode::ConflictState);
  FR_CHECK(again.reason() == fr::ReasonCode::AttemptTerminalAlready);
  FR_CHECK_EQ(rig.engine->stats().duplicate_completions, 1ull);
}

FR_TEST(adversarial, mismatched_idempotency_key_cannot_complete_an_attempt) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);

  fr::CompletionRequest completion;
  completion.attempt = dispatched->intents.front().attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = fr::Sha256Of(std::string_view("a different effect"));
  completion.target = fr::AttemptState::Applied;
  fr::ReconciliationOutcome outcome;
  const fr::Status status = rig.engine->Complete(completion, outcome);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::Rejected);
}

FR_TEST(adversarial, illegal_transition_is_refused) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);

  fr::CompletionRequest completion;
  completion.attempt = dispatched->intents.front().attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = dispatched->intents.front().idempotency_key;
  completion.target = fr::AttemptState::Verified;
  fr::ReconciliationOutcome outcome;
  const fr::Status status = rig.engine->Complete(completion, outcome);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::AttemptTransitionIllegal);
}

FR_TEST(adversarial, verification_must_be_independent_and_newer) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent& action = dispatched->intents.front();

  fr::CompletionRequest completion;
  completion.attempt = action.attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = action.idempotency_key;
  completion.target = fr::AttemptState::Applied;
  fr::ReconciliationOutcome outcome;
  FR_CHECK_STATUS_OK(rig.engine->Complete(completion, outcome));

  // The verifier publishes the confirming observation first.
  fr::ObservationSubmission verification =
      MakeObservation(kScope, "reporter/verifier", 1, 1, 0, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(verification, 1000, observation_result));

  fr::VerificationEvidence evidence;
  evidence.scope = RequireScope(kScope);
  evidence.subject = action.subject;
  evidence.idempotency_key = action.idempotency_key;
  evidence.observation = verification.observation;
  evidence.generation = fr::ObservationGeneration(1);
  evidence.received_epoch = rig.engine->epoch();
  evidence.verifier = RequireReporter("reporter/verifier");
  evidence.applier = RequireReporter("reporter/verifier");
  evidence.received_unix_ms = 1000;
  evidence.confirms_effect = true;

  fr::ReconciliationOutcome verified;
  const fr::Status not_independent = rig.engine->Verify(evidence, verified);
  FR_CHECK(!not_independent.ok());
  FR_CHECK(not_independent.reason() == fr::ReasonCode::VerificationNotIndependent);

  evidence.applier = RequireReporter("reporter/a");
  evidence.generation = action.authority.observation_generation;
  const fr::Status not_newer = rig.engine->Verify(evidence, verified);
  FR_CHECK(!not_newer.ok());
  FR_CHECK(not_newer.reason() == fr::ReasonCode::VerificationNotNewer);
}

FR_TEST(adversarial, verification_of_evidence_the_runtime_does_not_hold_is_refused) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent& action = dispatched->intents.front();

  fr::CompletionRequest completion;
  completion.attempt = action.attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = action.idempotency_key;
  completion.target = fr::AttemptState::Applied;
  fr::ReconciliationOutcome outcome;
  FR_CHECK_STATUS_OK(rig.engine->Complete(completion, outcome));

  fr::VerificationEvidence evidence;
  evidence.scope = RequireScope(kScope);
  evidence.subject = action.subject;
  evidence.idempotency_key = action.idempotency_key;
  evidence.observation = RequireObservation("observation/never-recorded");
  evidence.generation = fr::ObservationGeneration(9);
  evidence.received_epoch = rig.engine->epoch();
  evidence.verifier = RequireReporter("reporter/verifier");
  evidence.applier = RequireReporter("reporter/a");
  evidence.received_unix_ms = 1000;
  evidence.confirms_effect = true;
  fr::ReconciliationOutcome verified;
  const fr::Status status = rig.engine->Verify(evidence, verified);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::NotFound);
}

FR_TEST(adversarial, verification_requires_an_applied_attempt) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent& action = dispatched->intents.front();

  fr::ObservationSubmission verification =
      MakeObservation(kScope, "reporter/verifier", 1, 1, 0, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(verification, 1000, observation_result));

  fr::VerificationEvidence evidence;
  evidence.scope = RequireScope(kScope);
  evidence.subject = action.subject;
  evidence.idempotency_key = action.idempotency_key;
  evidence.observation = verification.observation;
  evidence.generation = fr::ObservationGeneration(1);
  evidence.received_epoch = rig.engine->epoch();
  evidence.verifier = RequireReporter("reporter/verifier");
  evidence.applier = RequireReporter("reporter/a");
  evidence.received_unix_ms = 1000;
  evidence.confirms_effect = true;
  fr::ReconciliationOutcome verified;
  const fr::Status status = rig.engine->Verify(evidence, verified);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::PreconditionFailed);
}

FR_TEST(adversarial, acknowledged_is_not_applied_and_applied_is_not_verified) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent& action = dispatched->intents.front();

  fr::CompletionRequest completion;
  completion.attempt = action.attempt;
  completion.epoch = rig.engine->epoch();
  completion.boot = rig.engine->boot();
  completion.idempotency_key = action.idempotency_key;
  completion.target = fr::AttemptState::Acknowledged;
  fr::ReconciliationOutcome outcome;
  FR_CHECK_STATUS_OK(rig.engine->Complete(completion, outcome));
  FR_CHECK(outcome.terminal_state == fr::AttemptState::Acknowledged);
  FR_CHECK(!outcome.verified);
  FR_CHECK(!outcome.ambiguous);

  completion.target = fr::AttemptState::Applied;
  FR_CHECK_STATUS_OK(rig.engine->Complete(completion, outcome));
  FR_CHECK(outcome.terminal_state == fr::AttemptState::Applied);
  FR_CHECK(!outcome.verified);

  const auto attempts = rig.engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  for (const fr::AttemptRecord& attempt : *attempts) {
    if (attempt.id == action.attempt) {
      FR_CHECK(attempt.state == fr::AttemptState::Applied);
      FR_CHECK(attempt.transitions.size() == 3);
    }
  }
}

FR_TEST(adversarial, incomparable_attribute_kinds_are_explicit_and_actionable) {
  // Two values of different kinds are never coerced into one another. They are
  // a proven difference, so the desired value remains well defined and the
  // decision stays actionable, but the reason names the incomparability rather
  // than pretending the values were merely unequal.
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  observation.subjects.begin()->second.observed[RequireAttribute("mtu")] =
      fr::AttributeValue::Text(std::string(fr::kMaxAttributeTextLength, 'x'));
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::Mismatched), std::size_t(1));
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(1));
  FR_CHECK(plan->decisions[0].drift == fr::DriftClass::Mismatched);
  FR_CHECK(plan->decisions[0].reason == fr::ReasonCode::ClassifiedAttributeNotComparable);
  FR_CHECK(plan->decisions[0].action == fr::ActionKind::ApplyDesired);
  FR_CHECK(plan->decisions[0].explanation.Has(fr::ReasonCode::ClassifiedAttributeMismatch));
}

FR_TEST(adversarial, a_value_outside_the_supported_kind_class_is_unsupported) {
  // The supported comparison class is the six declared attribute kinds. A value
  // outside it can never be produced by the document readers, but the
  // classifier must still refuse to approximate it: it reports UNSUPPORTED and
  // issues no action rather than treating the value as absent or as equal.
  const fr::SubjectId subject = RequireSubject("node/n1/port/eth0");
  fr::IntentDocument intent;
  intent.intent = RequireIntent("intent/unsupported");
  intent.scope = RequireScope(kScope);
  intent.definition = RequireDefinition("definition/unsupported");
  intent.generation = fr::IntentGeneration(1);
  intent.policy_version = fr::PolicyVersion(1);
  intent.evidence = fr::EvidenceClass::Synthetic;
  intent.complete = true;
  fr::SubjectIntent declared;
  declared.id = subject;
  declared.desired[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(9000);
  intent.subjects.emplace(subject, std::move(declared));
  intent.digest = fr::ComputeIntentDigest(intent);

  fr::ObservationDocument observation;
  observation.observation = RequireObservation("observation/unsupported/1");
  observation.scope = RequireScope(kScope);
  observation.reporter = RequireReporter("reporter/a");
  observation.generation = fr::ObservationGeneration(1);
  observation.evidence = fr::EvidenceClass::Synthetic;
  observation.complete = true;
  observation.authoritative_absence = true;
  observation.received_epoch = fr::CoordinatorEpoch(1);
  observation.received_unix_ms = 1000;
  fr::SubjectObservation seen;
  seen.id = subject;
  seen.observed[RequireAttribute("mtu")] = fr::AttributeValue();  // kind Invalid
  observation.subjects.emplace(subject, std::move(seen));
  observation.digest = fr::ComputeObservationDigest(observation);

  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  std::vector<const fr::IntentDocument*> intents{&intent};
  std::vector<const fr::ObservationDocument*> observations{&observation};
  fr::ClassificationInput input;
  input.scope = intent.scope;
  input.intents = &intents;
  input.observations = &observations;
  input.has_committed_intent = true;
  input.committed_intent_generation = intent.generation;
  input.policy = &policy;
  input.live_epoch = fr::CoordinatorEpoch(1);
  input.now_unix_ms = 1000;
  fr::ClassificationReport report;
  FR_CHECK_STATUS_OK(fr::ClassifyScope(input, report));
  FR_CHECK_EQ(report.records.size(), std::size_t(1));
  FR_CHECK(report.records[0].drift == fr::DriftClass::Unsupported);
  FR_CHECK(report.records[0].reason == fr::ReasonCode::SubjectKindUnsupported);
  FR_CHECK(fr::ToText(report.records[0].drift) == std::string("UNSUPPORTED"));
}

FR_TEST(adversarial, observation_for_the_wrong_scope_is_refused) {
  Rig rig;
  const fr::ObservationSubmission observation =
      MakeObservation("fabric/rack-9", "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  fr::ClassificationInput input;
  fr::IntentDocument foreign = MakeIntent("fabric/rack-9", 1, 1, fr::EvidenceClass::Synthetic);
  std::vector<const fr::IntentDocument*> intents{&foreign};
  input.scope = RequireScope(kScope);
  input.intents = &intents;
  input.observations = nullptr;
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  input.policy = &policy;
  input.live_epoch = rig.engine->epoch();
  fr::ClassificationReport report;
  FR_CHECK(!fr::ClassifyScope(input, report).ok());
}
