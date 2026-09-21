// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An independent downstream program. It uses only installed public headers and
// the exported target, performs a complete reconciliation cycle, and exits
// non-zero if any product-defining invariant does not hold.
//
// Everything it exercises is SYNTHETIC: deterministic in-process fixtures with
// no physical fabric element involved.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <summon/fabric_reconciliation/engine.hpp>
#include <summon/fabric_reconciliation/version.hpp>

namespace fr = summon::fabric_reconciliation;

namespace {

int Failure(const char* what) {
  std::fprintf(stderr, "consumer: FAILED: %s\n", what);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      (argc > 1) ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / "fabric-reconciliation-consumer";
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  std::printf("consumer: linked against %s %s\n", fr::kProductName, fr::kVersionString);

  fr::EngineOptions options;
  options.store_dir = directory;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = fr::ReconciliationEngine::Open(options, engine);
  if (!opened.ok()) {
    std::fprintf(stderr, "consumer: open failed: %s\n", opened.ToString().c_str());
    return 1;
  }
  if (engine->boot_report().freshness_restored ||
      engine->boot_report().mutation_authority_restored) {
    return Failure("a fresh boot claimed to have restored freshness or authority");
  }

  const auto scope = fr::ScopeId::TryParse("fabric/consumer");
  const auto subject = fr::SubjectId::TryParse("node/n1/port/eth0");
  const auto key = fr::AttributeKey::TryParse("mtu");
  if (!scope.has_value() || !subject.has_value() || !key.has_value()) {
    return Failure("identity construction failed");
  }

  fr::IntentDocument intent;
  intent.intent = *fr::IntentId::TryParse("intent/consumer");
  intent.scope = *scope;
  intent.definition = *fr::DefinitionId::TryParse("definition/consumer");
  intent.generation = fr::IntentGeneration(1);
  intent.policy_version = fr::PolicyVersion(1);
  intent.evidence = fr::EvidenceClass::Synthetic;
  intent.complete = true;
  fr::SubjectIntent declared;
  declared.id = *subject;
  declared.desired.emplace(*key, fr::AttributeValue::Unsigned(9000));
  intent.subjects.emplace(*subject, std::move(declared));

  fr::IntentCommitResult commit_result;
  if (!engine->CommitIntent(intent, commit_result).ok()) {
    return Failure("intent commit failed");
  }

  fr::ObservationSubmission observation;
  observation.observation = *fr::ObservationId::TryParse("observation/consumer/1");
  observation.scope = *scope;
  observation.reporter = *fr::ReporterId::TryParse("reporter/consumer");
  observation.generation = fr::ObservationGeneration(1);
  observation.evidence = fr::EvidenceClass::Synthetic;
  observation.complete = true;
  observation.authoritative_absence = true;
  fr::SubjectObservation seen;
  seen.id = *subject;
  seen.observed.emplace(*key, fr::AttributeValue::Unsigned(1500));
  observation.subjects.emplace(*subject, std::move(seen));

  fr::ObservationCommitResult observation_result;
  if (!engine->RecordObservationAt(observation, 1000, observation_result).ok()) {
    return Failure("observation commit failed");
  }

  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  if (!plan.ok()) {
    return Failure("plan failed");
  }
  if (plan->verdict != fr::ConvergenceVerdict::ConvergenceRequired) {
    return Failure("expected a convergence-required verdict");
  }
  if (plan->MutationCount() != 1) {
    return Failure("expected exactly one action");
  }
  std::printf("consumer: plan %s requires %zu action\n", plan->plan_id.c_str(),
              plan->MutationCount());

  const auto dispatched = engine->Dispatch(*plan);
  if (!dispatched.ok() || dispatched->intents.size() != 1) {
    return Failure("dispatch failed");
  }

  // Re-dispatching the same plan must not issue a second attempt.
  const auto again = engine->Dispatch(*plan);
  if (!again.ok() || again->already_issued != 1 || again->intents.size() != 1) {
    return Failure("dispatch is not idempotent");
  }

  fr::CompletionRequest completion;
  completion.attempt = dispatched->intents.front().attempt;
  completion.epoch = engine->epoch();
  completion.boot = engine->boot();
  completion.idempotency_key = dispatched->intents.front().idempotency_key;
  completion.target = fr::AttemptState::Applied;
  fr::ReconciliationOutcome outcome;
  if (!engine->Complete(completion, outcome).ok()) {
    return Failure("completion failed");
  }
  if (outcome.verified) {
    return Failure("an application was reported as verified without verification evidence");
  }

  // A clean restart must advance the epoch and refuse the pre-restart
  // completion, which is the behaviour the release exists to guarantee.
  engine.reset();
  std::unique_ptr<fr::ReconciliationEngine> reopened;
  if (!fr::ReconciliationEngine::Open(options, reopened).ok()) {
    return Failure("reopen failed");
  }
  if (reopened->epoch().value() <= completion.epoch.value()) {
    return Failure("restart did not advance the coordinator epoch");
  }
  fr::ReconciliationOutcome late;
  if (reopened->Complete(completion, late).ok()) {
    return Failure("a pre-restart completion was accepted after restart");
  }
  fr::PlanRequest after;
  after.scope = *scope;
  after.now_unix_ms = 1000;
  const auto stale = reopened->Plan(after);
  if (!stale.ok() || stale->MutationCount() != 0) {
    return Failure("a restarted runtime acted on evidence it did not revalidate");
  }
  if (stale->verdict != fr::ConvergenceVerdict::Indeterminate) {
    return Failure("a restarted runtime claimed convergence without fresh evidence");
  }

  std::printf("consumer: OK\n");
  std::filesystem::remove_all(directory, error);
  return 0;
}
