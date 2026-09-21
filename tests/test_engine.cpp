// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Restart semantics in process: what survives, what must not survive, and what
// must be surfaced as interrupted rather than assumed complete. The real
// process-kill proof lives in test_multiprocess.cpp.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

const char* kScope = "fabric/rack-7";

struct Seeded {
  fr::CoordinatorEpoch epoch;
  fr::BootId boot;
  std::size_t attempts{0};
  std::vector<fr::ActionIntent> intents;
};

Seeded SeedAndDispatch(const std::filesystem::path& directory) {
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(directory);
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 6, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 6, 2, false, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);

  Seeded seeded;
  seeded.epoch = engine->epoch();
  seeded.boot = engine->boot();
  seeded.attempts = dispatched->intents.size();
  seeded.intents = dispatched->intents;
  return seeded;
}

}  // namespace

FR_TEST(restart, advances_the_epoch_and_changes_the_boot_identity) {
  ScratchDirectory scratch{"engine-restart"};
  const Seeded seeded = SeedAndDispatch(scratch.path());
  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(scratch.path());
  FR_CHECK(reopened->epoch().value() == seeded.epoch.value() + 1);
  FR_CHECK(!(reopened->boot() == seeded.boot));
  FR_CHECK_EQ(reopened->boot_report().previous_epoch.value(), seeded.epoch.value());
  FR_CHECK(reopened->boot_report().epoch.value() == reopened->epoch().value());
}

FR_TEST(restart, never_restores_freshness_or_mutation_authority) {
  ScratchDirectory scratch{"engine-restart-freshness"};
  SeedAndDispatch(scratch.path());
  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(scratch.path());
  FR_CHECK(!reopened->boot_report().freshness_restored);
  FR_CHECK(!reopened->boot_report().mutation_authority_restored);

  // The retained observation is still evidence, but it is not fresh.
  const auto evidence = reopened->ObservationEvidence(RequireScope(kScope));
  FR_CHECK_STATUS_OK(evidence);
  FR_CHECK_EQ(evidence->size(), std::size_t(1));
  FR_CHECK(evidence->front().received_epoch.value() < reopened->epoch().value());

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = reopened->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
  FR_CHECK(plan->freshness == fr::FreshnessVerdict::ForeignEpoch);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::StaleObservation), std::size_t(6));
}

FR_TEST(restart, in_flight_attempts_are_interrupted_and_flagged_ambiguous) {
  ScratchDirectory scratch{"engine-restart-attempts"};
  const Seeded seeded = SeedAndDispatch(scratch.path());
  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(scratch.path());
  FR_CHECK_EQ(reopened->boot_report().attempts_interrupted, seeded.attempts);
  FR_CHECK_EQ(reopened->boot_report().attempts_fenced, seeded.attempts);

  const auto attempts = reopened->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), seeded.attempts);
  for (const fr::AttemptRecord& attempt : *attempts) {
    FR_CHECK(attempt.state == fr::AttemptState::Interrupted);
    FR_CHECK(attempt.epoch.value() == seeded.epoch.value());
  }
  const auto outcomes = reopened->Outcomes(RequireScope(kScope));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK_EQ(outcomes->size(), seeded.attempts);
  for (const fr::ReconciliationOutcome& outcome : *outcomes) {
    FR_CHECK(outcome.ambiguous);
    FR_CHECK(!outcome.verified);
    FR_CHECK(outcome.terminal_state == fr::AttemptState::Interrupted);
  }
}

FR_TEST(restart, an_issued_but_never_dispatched_attempt_is_abandoned_not_ambiguous) {
  // This is the precise distinction the runtime makes: an attempt that never
  // left the process cannot have produced an effect, so it is abandoned and not
  // reported as ambiguous.
  ScratchDirectory scratch{"engine-restart-issued"};
  {
    fr::EngineOptions options;
    options.store_dir = scratch.path();
    std::unique_ptr<fr::ReconciliationEngine> engine;
    FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));
  }
  // Manufacture an Issued attempt directly through the durable store. The
  // store lock is exclusive, so the store is closed before the engine opens.
  {
    fr::DurableStore store;
    fr::StoreOpenReport report;
    FR_CHECK_STATUS_OK(
        fr::DurableStore::Open(scratch.path(), fr::RuntimeLimits{}, true, false, store, report));
    fr::AttemptRecord attempt;
    attempt.id = fr::AttemptId(store.state().next_attempt.value());
    attempt.idempotency_key = fr::Sha256Of(std::string_view("issued-only"));
    attempt.scope = RequireScope(kScope);
    attempt.subject = RequireSubject("node/n1/port/eth0");
    attempt.action = fr::ActionKind::ApplyDesired;
    attempt.epoch = fr::CoordinatorEpoch(1);
    attempt.boot = fr::BootId::Generate();
    attempt.state = fr::AttemptState::Issued;
    attempt.authority.coordinator_epoch = fr::CoordinatorEpoch(1);
    attempt.authority.boot = attempt.boot;
    attempt.authority.policy_id = store.state().policy.id;
    attempt.authority.policy_version = store.state().policy.version;
    attempt.authority.policy_digest = fr::ComputePolicyDigest(store.state().policy);
    attempt.authority.definition = RequireDefinition("definition/test");
    attempt.authority.intent = RequireIntent("intent/test");
    attempt.authority.intent_generation = fr::IntentGeneration(1);
    FR_CHECK_STATUS_OK(store.IssueAttempt(attempt));
  }

  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  FR_CHECK_EQ(engine->boot_report().attempts_abandoned, std::size_t(1));
  FR_CHECK_EQ(engine->boot_report().attempts_interrupted, std::size_t(0));
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), std::size_t(1));
  FR_CHECK(attempts->front().state == fr::AttemptState::Abandoned);
  const auto outcomes = engine->Outcomes(RequireScope(kScope));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK_EQ(outcomes->size(), std::size_t(1));
  FR_CHECK(!outcomes->front().ambiguous);
}

FR_TEST(restart, intent_and_policy_survive_but_are_not_replaced) {
  ScratchDirectory scratch{"engine-restart-intent"};
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    const fr::IntentDocument intent = MakeIntent(kScope, 5, 3, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
    fr::ReconciliationPolicy policy = fr::DefaultPolicy();
    policy.version = fr::PolicyVersion(4);
    policy.permit_withdraw_unexpected = false;
    FR_CHECK_STATUS_OK(engine->PutPolicy(policy));
  }
  fr::EngineOptions options;
  options.store_dir = scratch.path();
  // A different initial policy must not overwrite the durable one.
  options.initial_policy = fr::DefaultPolicy();
  std::unique_ptr<fr::ReconciliationEngine> engine;
  FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));
  const auto intent = engine->CurrentIntent(RequireScope(kScope));
  FR_CHECK_STATUS_OK(intent);
  FR_CHECK_EQ(intent->generation.value(), 5ull);
  const auto policy = engine->ActivePolicy();
  FR_CHECK_STATUS_OK(policy);
  FR_CHECK_EQ(policy->version.value(), 4ull);
  FR_CHECK(!policy->permit_withdraw_unexpected);
}

FR_TEST(restart, three_consecutive_restarts_advance_the_epoch_monotonically) {
  ScratchDirectory scratch{"engine-restart-three"};
  std::uint64_t previous = 0;
  for (int iteration = 0; iteration < 3; ++iteration) {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    if (iteration > 0) {
      FR_CHECK(engine->epoch().value() > previous);
    }
    previous = engine->epoch().value();
  }
  FR_CHECK_EQ(previous, 3ull);
}

FR_TEST(restart, a_fence_survives_and_keeps_revoking) {
  ScratchDirectory scratch{"engine-restart-fence"};
  fr::FenceId fence_id;
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    fr::FenceEntry fence;
    fence_id = *fr::FenceId::TryParse("fence/persistent");
    fence.id = fence_id;
    fence.scope = RequireScope(kScope);
    fence.fenced_below = fr::CoordinatorEpoch(engine->epoch().value() + 1);
    fence.reason = fr::ReasonCode::ScopeFenced;
    FR_CHECK_STATUS_OK(engine->PutFence(fence));
  }
  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(scratch.path());
  FR_CHECK_EQ(reopened->boot_report().fences_active, std::size_t(1));
  const auto fences = reopened->ActiveFences(RequireScope(kScope));
  FR_CHECK_STATUS_OK(fences);
  FR_CHECK_EQ(fences->size(), std::size_t(1));
  FR_CHECK_STATUS_OK(reopened->ClearFence(fence_id));
  const auto cleared = reopened->ActiveFences(RequireScope(kScope));
  FR_CHECK_STATUS_OK(cleared);
  FR_CHECK_EQ(cleared->size(), std::size_t(0));
}

FR_TEST(restart, read_only_opens_never_mutate_the_store) {
  ScratchDirectory scratch{"engine-read-only"};
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    const fr::IntentDocument intent = MakeIntent(kScope, 1, 2, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
  }
  const std::uintmax_t before = std::filesystem::file_size(scratch.path() / "store.journal");

  fr::EngineOptions options;
  options.store_dir = scratch.path();
  options.read_only = true;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));
  FR_CHECK(engine->boot_report().epoch.value() == engine->boot_report().previous_epoch.value());
  const fr::IntentDocument intent = MakeIntent(kScope, 2, 2, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult result;
  FR_CHECK(!engine->CommitIntent(intent, result).ok());
  FR_CHECK(!engine->Compact().ok());
  FR_CHECK_EQ(std::filesystem::file_size(scratch.path() / "store.journal"), before);
}

FR_TEST(restart, a_fresh_clone_from_persisted_state_reproduces_the_plan) {
  // This is the fresh-clone closure scenario in process: a new engine opened on
  // an existing store, with a new observation recorded in the new incarnation,
  // must classify from the persisted intended state and produce a
  // deterministic plan.
  ScratchDirectory scratch{"engine-fresh-clone"};
  std::string first_plan_id;
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    const fr::IntentDocument intent = MakeIntent(kScope, 1, 12, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
    FR_CHECK_STATUS_OK(engine->Compact());
  }
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    const fr::ObservationSubmission observation =
        MakeObservation(kScope, "reporter/fresh", 1, 12, 3, false, fr::EvidenceClass::Synthetic);
    fr::ObservationCommitResult result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, result));
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    FR_CHECK(plan->verdict == fr::ConvergenceVerdict::ConvergenceRequired);
    FR_CHECK_EQ(plan->intent_generation.value(), 1ull);
    FR_CHECK(plan->MutationCount() > 0);
    first_plan_id = plan->plan_id;

    const auto intents = engine->CurrentIntent(RequireScope(kScope));
    FR_CHECK_STATUS_OK(intents);
    FR_CHECK_EQ(intents->subjects.size(), std::size_t(12));
  }
  {
    // A third incarnation, with the same observation evidence persisted but not
    // fresh: the plan must degrade to indeterminate rather than claim anything.
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
    FR_CHECK(!(plan->plan_id == first_plan_id));
  }
}
