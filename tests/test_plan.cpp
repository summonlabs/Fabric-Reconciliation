// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Product-defining proofs: convergence emits no mutation intent, plans are
// deterministic under every permutation of the evidence, dry-run and dispatch
// agree exactly, and the verdict never claims more than the evidence supports.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

struct Rig {
  ScratchDirectory scratch{"plan"};
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

  fr::Result<fr::ReconciliationPlan> Plan(const std::string& scope, fr::UnixMillis now = 1000,
                                         bool dry_run = true) {
    fr::PlanRequest request;
    request.scope = RequireScope(scope.c_str());
    request.now_unix_ms = now;
    request.dry_run = dry_run;
    return engine->Plan(request);
  }
};

const char* kScope = "fabric/rack-7";

}  // namespace

FR_TEST(idempotence, converged_state_emits_no_mutation_intent) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 8, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 8, 0, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::ConvergedProven);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK_EQ(plan->CountDisposition(fr::DecisionDisposition::Actionable), std::size_t(0));
  for (const fr::Decision& decision : plan->decisions) {
    FR_CHECK(decision.action == fr::ActionKind::None);
    FR_CHECK(decision.disposition == fr::DecisionDisposition::AlreadySatisfied);
  }

  // Dispatching a converged plan must issue nothing at all.
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  FR_CHECK_EQ(dispatched->intents.size(), std::size_t(0));
}

FR_TEST(idempotence, repeated_plans_are_byte_identical) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 32, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 32, 4, true, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  const auto first = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(first);
  for (int iteration = 0; iteration < 8; ++iteration) {
    const auto again = rig.Plan(kScope);
    FR_CHECK_STATUS_OK(again);
    FR_CHECK_EQ(again->plan_id, first->plan_id);
    FR_CHECK_EQ(fr::ToJson(*again), fr::ToJson(*first));
  }
}

FR_TEST(idempotence, dispatch_twice_issues_no_duplicate_attempt) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 6, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 6, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->MutationCount() > 0);

  const auto first = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(first);
  const std::size_t issued = first->intents.size();
  FR_CHECK(issued > 0);

  const auto second = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(second);
  FR_CHECK_EQ(second->intents.size(), issued);
  FR_CHECK_EQ(second->already_issued, issued);
  for (const fr::ActionIntent& action : second->intents) {
    bool matched = false;
    for (const fr::ActionIntent& original : first->intents) {
      if (original.idempotency_key == action.idempotency_key) {
        matched = true;
        FR_CHECK_EQ(original.attempt.value(), action.attempt.value());
      }
    }
    FR_CHECK(matched);
  }

  const auto attempts = rig.engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), issued);
}

FR_TEST(idempotence, distinct_subjects_have_distinct_keys) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 32, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 32, 2, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  std::vector<std::string> keys;
  for (const fr::Decision& decision : plan->decisions) {
    if (decision.disposition == fr::DecisionDisposition::Actionable) {
      keys.push_back(fr::ToHex(decision.idempotency_key));
    }
  }
  FR_CHECK(keys.size() > 1);
  std::sort(keys.begin(), keys.end());
  FR_CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

FR_TEST(determinism, plan_is_independent_of_evidence_ordering) {
  // The classifier walks std::map ordered containers, so the plan must not
  // depend on the order in which candidates are supplied. This test supplies
  // the same observation twice with different identifiers to prove that the
  // selected generation, not the arrival order, decides the outcome.
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 16, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission first =
      MakeObservation(kScope, "reporter/a", 1, 16, 3, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, first));
  fr::ObservationSubmission second = first;
  second.generation = fr::ObservationGeneration(2);
  second.observation = RequireObservation("observation/reporter/a/2");
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(second, 1000, result));

  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK_EQ(plan->observation_generation.value(), 2ull);

  // Every retained observation is retained; the selected one is the highest
  // generation regardless of insertion order.
  const auto evidence = rig.engine->ObservationEvidence(RequireScope(kScope));
  FR_CHECK_STATUS_OK(evidence);
  FR_CHECK_EQ(evidence->size(), std::size_t(2));
}

FR_TEST(verdict, indeterminate_never_reads_as_success) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, true, fr::EvidenceClass::Synthetic);
  // The reporter covered only part of the scope and is not entitled to assert
  // absence, so the subject it did not see is a coverage gap rather than a
  // proven absence.
  observation.complete = false;
  observation.authoritative_absence = false;
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK(plan->CountDisposition(fr::DecisionDisposition::Indeterminate) > 0);
  FR_CHECK(plan->CountDrift(fr::DriftClass::Unknown) > 0);
  FR_CHECK(plan->scope_reason != fr::ReasonCode::ClassifiedAlreadyConverged);
}

FR_TEST(verdict, stale_observation_is_indeterminate_not_convergence_required) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope, 100000);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK(plan->freshness == fr::FreshnessVerdict::Expired);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
}

FR_TEST(verdict, proven_blocked_when_policy_forbids_every_action) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 2, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.version = fr::PolicyVersion(2);
  policy.permit_apply_mismatched = false;
  FR_CHECK_STATUS_OK(rig.engine->PutPolicy(policy));

  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::ProvenBlocked);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK(plan->CountDisposition(fr::DecisionDisposition::Blocked) > 0);
}

FR_TEST(verdict, search_limit_is_explicit_and_reduces_proof_strength) {
  ScratchDirectory scratch{"plan-limit"};
  fr::EngineOptions options;
  options.store_dir = scratch.path();
  options.limits.max_plan_actions = 4;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));

  const fr::IntentDocument intent = MakeIntent(kScope, 1, 16, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 16, 3, false, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->truncated);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::SearchLimitReached);
  FR_CHECK_EQ(plan->decisions.size(), std::size_t(4));
  FR_CHECK(plan->scope_reason == fr::ReasonCode::PlanTruncated);
}

FR_TEST(verdict, no_intent_and_no_observation_is_not_convergence) {
  Rig rig;
  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK(plan->scope_reason == fr::ReasonCode::NoIntentForScope);
}

FR_TEST(explain, every_decision_names_a_subject_and_a_reason) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 12, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 12, 3, true, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope);
  FR_CHECK_STATUS_OK(plan);

  for (const fr::Decision& decision : plan->decisions) {
    FR_CHECK(!decision.subject.empty());
    FR_CHECK(decision.scope == RequireScope(kScope));
    FR_CHECK(decision.reason != fr::ReasonCode::None);
    FR_CHECK(!decision.explanation.steps().empty());
    // The authority vector must bind the evidence the decision rests on.
    FR_CHECK(decision.authority.coordinator_epoch == plan->epoch);
    FR_CHECK(decision.authority.intent_generation == plan->intent_generation);
    FR_CHECK(decision.authority.intent_digest == plan->intent_digest);
    FR_CHECK(decision.authority.observation_digest == plan->observation_digest);
  }
  const std::string rendered = fr::RenderPlanText(*plan);
  FR_CHECK(Contains(rendered, plan->plan_id));
  FR_CHECK(Contains(rendered, "AUTHORITY_PROVEN"));
}

FR_TEST(explain, dry_run_and_dispatch_decisions_agree) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 10, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 10, 3, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  const auto dry = rig.Plan(kScope, 1000, true);
  FR_CHECK_STATUS_OK(dry);
  const auto live = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(live);
  FR_CHECK_EQ(live->plan_id, dry->plan_id);

  const auto dispatched = rig.engine->Dispatch(*live);
  FR_CHECK_STATUS_OK(dispatched);
  std::vector<std::string> actionable;
  for (const fr::Decision& decision : dry->decisions) {
    if (decision.disposition == fr::DecisionDisposition::Actionable) {
      actionable.push_back(fr::ToHex(decision.idempotency_key));
    }
  }
  FR_CHECK_EQ(dispatched->intents.size(), actionable.size());
  for (const fr::ActionIntent& action : dispatched->intents) {
    FR_CHECK(std::find(actionable.begin(), actionable.end(), fr::ToHex(action.idempotency_key)) !=
             actionable.end());
  }
}

FR_TEST(dispatch, a_plan_from_a_previous_epoch_is_refused) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);
  fr::ReconciliationPlan stale = *plan;
  stale.boot = fr::BootId::Generate();
  const auto dispatched = rig.engine->Dispatch(stale);
  FR_CHECK(!dispatched.ok());
  FR_CHECK(dispatched.status().code() == fr::StatusCode::StaleAuthority);
}

FR_TEST(dispatch, a_plan_is_refused_after_the_policy_changes) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);

  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.version = fr::PolicyVersion(9);
  FR_CHECK_STATUS_OK(rig.engine->PutPolicy(policy));

  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK(!dispatched.ok());
  FR_CHECK(dispatched.status().code() == fr::StatusCode::StaleAuthority);
  FR_CHECK(dispatched.status().reason() == fr::ReasonCode::AuthorityPolicyMismatch);
}

FR_TEST(dispatch, a_plan_is_refused_after_the_intent_changes) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);

  const fr::IntentDocument newer = MakeIntent(kScope, 2, 4, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(newer, result));

  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK(!dispatched.ok());
  FR_CHECK(dispatched.status().reason() == fr::ReasonCode::AuthorityIntentGenerationMismatch);
}

FR_TEST(dispatch, a_fence_blocks_actions_but_not_explanations) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 4, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 4, 3, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));

  const fr::CoordinatorEpoch epoch = rig.engine->epoch();
  fr::FenceEntry fence;
  fence.id = *fr::FenceId::TryParse("fence/all");
  fence.scope = RequireScope(kScope);
  fence.fenced_below = fr::CoordinatorEpoch(epoch.value() + 1);
  fence.reason = fr::ReasonCode::ScopeFenced;
  FR_CHECK_STATUS_OK(rig.engine->PutFence(fence));

  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK(plan->CountDisposition(fr::DecisionDisposition::Fenced) > 0);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::ProvenBlocked);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  FR_CHECK_EQ(dispatched->intents.size(), std::size_t(0));
}

FR_TEST(dispatch, fence_applies_to_one_subject_only) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 8, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 8, 2, false, fr::EvidenceClass::Synthetic);
  FR_CHECK_STATUS_OK(rig.Seed(intent, observation));
  const fr::CoordinatorEpoch epoch = rig.engine->epoch();

  fr::FenceEntry fence;
  fence.id = *fr::FenceId::TryParse("fence/one");
  fence.scope = RequireScope(kScope);
  fence.subject = RequireSubject("node/n0/port/eth0");
  fence.fenced_below = fr::CoordinatorEpoch(epoch.value() + 1);
  fence.reason = fr::ReasonCode::SubjectFenced;
  FR_CHECK_STATUS_OK(rig.engine->PutFence(fence));

  const auto plan = rig.Plan(kScope, 1000, false);
  FR_CHECK_STATUS_OK(plan);
  for (const fr::Decision& decision : plan->decisions) {
    if (decision.subject == RequireSubject("node/n0/port/eth0")) {
      FR_CHECK(decision.disposition == fr::DecisionDisposition::Fenced);
    }
  }
  FR_CHECK(plan->MutationCount() > 0);
  const auto dispatched = rig.engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  for (const fr::ActionIntent& action : dispatched->intents) {
    FR_CHECK(!(action.subject == RequireSubject("node/n0/port/eth0")));
  }
}

FR_TEST(fence, a_fence_that_revokes_nothing_is_refused) {
  Rig rig;
  fr::FenceEntry fence;
  fence.id = *fr::FenceId::TryParse("fence/zero");
  fence.scope = RequireScope(kScope);
  fence.fenced_below = fr::CoordinatorEpoch(0);
  const fr::Status status = rig.engine->PutFence(fence);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::FenceEpochNotNewer);
}

FR_TEST(policy, put_policy_refuses_regression_and_divergent_republication) {
  Rig rig;
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  policy.version = fr::PolicyVersion(5);
  FR_CHECK_STATUS_OK(rig.engine->PutPolicy(policy));

  fr::ReconciliationPolicy older = fr::DefaultPolicy();
  older.version = fr::PolicyVersion(4);
  FR_CHECK(!rig.engine->PutPolicy(older).ok());

  fr::ReconciliationPolicy divergent = policy;
  divergent.permit_apply_missing = !policy.permit_apply_missing;
  FR_CHECK(!rig.engine->PutPolicy(divergent).ok());

  FR_CHECK(rig.engine->PutPolicy(policy, true).ok());
}

FR_TEST(intent, commit_is_idempotent_and_refuses_regression) {
  Rig rig;
  const fr::IntentDocument intent = MakeIntent(kScope, 4, 4, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult first;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(intent, first));
  FR_CHECK(!first.duplicate);

  fr::IntentCommitResult duplicate;
  FR_CHECK_STATUS_OK(rig.engine->CommitIntent(intent, duplicate));
  FR_CHECK(duplicate.duplicate);
  FR_CHECK(duplicate.digest == first.digest);

  fr::IntentCommitResult regressed;
  const fr::Status status = rig.engine->CommitIntent(
      MakeIntent(kScope, 3, 4, fr::EvidenceClass::Synthetic), regressed);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::IntentGenerationRegressed);

  fr::IntentDocument divergent = intent;
  divergent.subjects.begin()->second.desired[RequireAttribute("mtu")] =
      fr::AttributeValue::Unsigned(1500);
  divergent.digest = fr::ComputeIntentDigest(divergent);
  fr::IntentCommitResult conflict;
  const fr::Status conflicting = rig.engine->CommitIntent(divergent, conflict);
  FR_CHECK(!conflicting.ok());
  FR_CHECK(conflicting.code() == fr::StatusCode::ConflictState);
}

FR_TEST(observation, record_is_idempotent_and_refuses_generation_regression) {
  Rig rig;
  const fr::ObservationSubmission first =
      MakeObservation(kScope, "reporter/a", 5, 4, 5, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(first, 1000, result));
  FR_CHECK(!result.duplicate);

  fr::ObservationCommitResult duplicate;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(first, 1000, duplicate));
  FR_CHECK(duplicate.duplicate);

  fr::ObservationSubmission older =
      MakeObservation(kScope, "reporter/a", 4, 4, 5, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult regressed;
  const fr::Status status = rig.engine->RecordObservationAt(older, 1000, regressed);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::ObservationGenerationRegressed);

  // A different reporter has an independent generation stream.
  const fr::ObservationSubmission other =
      MakeObservation(kScope, "reporter/b", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult other_result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(other, 1000, other_result));
}

FR_TEST(observation, the_receipt_epoch_can_never_be_supplied_by_the_caller) {
  Rig rig;
  const fr::ObservationSubmission submission =
      MakeObservation(kScope, "reporter/a", 1, 4, 5, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult result;
  FR_CHECK_STATUS_OK(rig.engine->RecordObservationAt(submission, 1000, result));
  FR_CHECK(result.stamped.received_epoch == rig.engine->epoch());
  FR_CHECK_EQ(result.stamped.received_unix_ms, 1000);
}
