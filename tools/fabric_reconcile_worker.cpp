// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Multiprocess proof worker.
//
// This program exists so that the release can prove restart behaviour with a
// real, separately scheduled operating system process that is terminated at a
// chosen durable boundary. The boundaries are named after the ordering the
// runtime claims: a hard exit before the durable append, after the durable
// append but before the acknowledgement, and after an effect may have reached
// the fabric but before any completion was recorded.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/document_json.hpp"
#include "summon/fabric_reconciliation/engine.hpp"

namespace fr = summon::fabric_reconciliation;

namespace {

[[noreturn]] void Die(const char* message, int code) {
  std::fprintf(stderr, "%s\n", message);
  std::exit(code);
}

/// Terminates the process without unwinding, flushing or running destructors.
/// This is the strongest available simulation of a power loss or a kill.
[[noreturn]] void HardExit(int code) {
  std::fflush(nullptr);
  std::_Exit(code);
}

struct Arguments {
  std::string store;
  std::string mode;
  std::string scope{"fabric/rack-7"};
  std::string kill_at;
  long subjects{8};
  long seed{1};
  long now{1000};
  bool verbose{false};
};

std::string Require(const std::vector<std::string>& values, std::size_t index) {
  if (index >= values.size()) {
    Die("missing value for argument", 2);
  }
  return values[index];
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  std::vector<std::string> raw;
  for (int index = 1; index < argc; ++index) {
    raw.emplace_back(argv[index]);
  }
  for (std::size_t index = 0; index < raw.size(); ++index) {
    const std::string& token = raw[index];
    if (token == "--store") { arguments.store = Require(raw, ++index); }
    else if (token == "--mode") { arguments.mode = Require(raw, ++index); }
    else if (token == "--scope") { arguments.scope = Require(raw, ++index); }
    else if (token == "--kill-at") { arguments.kill_at = Require(raw, ++index); }
    else if (token == "--subjects") { arguments.subjects = std::strtol(Require(raw, ++index).c_str(), nullptr, 10); }
    else if (token == "--seed") { arguments.seed = std::strtol(Require(raw, ++index).c_str(), nullptr, 10); }
    else if (token == "--now") { arguments.now = std::strtol(Require(raw, ++index).c_str(), nullptr, 10); }
    else if (token == "--verbose") { arguments.verbose = true; }
    else { Die("unknown argument", 2); }
  }
  if (arguments.store.empty() || arguments.mode.empty()) {
    Die("--store and --mode are required", 2);
  }

  fr::EngineOptions options;
  options.store_dir = std::filesystem::path(arguments.store);
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = fr::ReconciliationEngine::Open(options, engine);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.ToString().c_str());
    return 3;
  }

  const auto scope = fr::ScopeId::TryParse(arguments.scope);
  if (!scope.has_value()) {
    Die("invalid scope", 2);
  }

  const auto build_subject = [](long index) {
    return "node/n" + std::to_string(index) + "/port/eth0";
  };

  if (arguments.mode == "seed") {
    fr::IntentDocument document;
    const auto intent_id = fr::IntentId::TryParse("intent/worker-seed");
    const auto definition = fr::DefinitionId::TryParse("definition/worker");
    if (!intent_id.has_value() || !definition.has_value()) {
      Die("invalid identity", 2);
    }
    document.intent = *intent_id;
    document.scope = *scope;
    document.definition = *definition;
    document.generation = fr::IntentGeneration(1);
    document.policy_version = fr::PolicyVersion(1);
    document.evidence = fr::EvidenceClass::Synthetic;
    document.complete = true;
    for (long index = 0; index < arguments.subjects; ++index) {
      fr::SubjectIntent entry;
      const auto subject = fr::SubjectId::TryParse(build_subject(index));
      if (!subject.has_value()) {
        Die("invalid subject", 2);
      }
      entry.id = *subject;
      entry.desired.emplace(*fr::AttributeKey::TryParse("mtu"), fr::AttributeValue::Unsigned(9000));
      document.subjects.emplace(*subject, std::move(entry));
    }
    fr::IntentCommitResult result;
    const fr::Status status = engine->CommitIntent(document, result);
    if (!status.ok()) {
      std::fprintf(stderr, "commit intent failed: %s\n", status.ToString().c_str());
      return 4;
    }
    std::printf("seeded subjects=%ld digest=%s\n", arguments.subjects,
                fr::ToHex(result.digest).c_str());
    return 0;
  }

  if (arguments.mode == "observe") {
    fr::ObservationSubmission submission;
    const auto observation = fr::ObservationId::TryParse("observation/worker/1");
    const auto reporter = fr::ReporterId::TryParse("reporter/worker");
    if (!observation.has_value() || !reporter.has_value()) {
      Die("invalid identity", 2);
    }
    submission.observation = *observation;
    submission.scope = *scope;
    submission.reporter = *reporter;
    submission.evidence = fr::EvidenceClass::Synthetic;
    submission.complete = true;
    submission.authoritative_absence = true;
    for (long index = 0; index < arguments.subjects; ++index) {
      fr::SubjectObservation entry;
      const auto subject = fr::SubjectId::TryParse(build_subject(index));
      if (!subject.has_value()) {
        Die("invalid subject", 2);
      }
      entry.id = *subject;
      const unsigned value = ((index % 3) == 0) ? 1500u : 9000u;
      entry.observed.emplace(*fr::AttributeKey::TryParse("mtu"), fr::AttributeValue::Unsigned(value));
      submission.subjects.emplace(*subject, std::move(entry));
    }
    fr::ObservationCommitResult result;
    const fr::Status status = engine->RecordObservationAt(submission, arguments.now, result);
    if (!status.ok()) {
      std::fprintf(stderr, "record observation failed: %s\n", status.ToString().c_str());
      return 5;
    }
    std::printf("observed subjects=%ld epoch=%llu\n", arguments.subjects,
                static_cast<unsigned long long>(engine->epoch().value()));
    return 0;
  }

  if (arguments.mode == "reconcile") {
    fr::PlanRequest request;
    request.scope = *scope;
    request.now_unix_ms = arguments.now;
    const auto plan = engine->Plan(request);
    if (!plan.ok()) {
      std::fprintf(stderr, "plan failed: %s\n", plan.status().ToString().c_str());
      return 6;
    }
    std::printf("plan id=%s verdict=%s actions=%zu\n", plan->plan_id.c_str(),
                fr::ToText(plan->verdict), plan->MutationCount());
    if (arguments.kill_at == "before_dispatch") {
      HardExit(70);
    }
    const auto dispatched = engine->Dispatch(*plan);
    if (!dispatched.ok()) {
      std::fprintf(stderr, "dispatch failed: %s\n", dispatched.status().ToString().c_str());
      return 7;
    }
    std::printf("dispatched=%zu\n", dispatched->intents.size());
    if (dispatched->intents.empty()) {
      return 0;
    }
    if (arguments.kill_at == "after_dispatch_before_ack") {
      HardExit(71);
    }
    fr::CompletionRequest completion;
    completion.attempt = dispatched->intents.front().attempt;
    completion.epoch = engine->epoch();
    completion.boot = engine->boot();
    completion.idempotency_key = dispatched->intents.front().idempotency_key;
    completion.target = fr::AttemptState::Applied;
    completion.at_unix_ms = arguments.now + 10;
    fr::ReconciliationOutcome outcome;
    if (arguments.kill_at == "before_completion_commit") {
      HardExit(72);
    }
    const fr::Status completed = engine->Complete(completion, outcome);
    if (!completed.ok()) {
      std::fprintf(stderr, "complete failed: %s\n", completed.ToString().c_str());
      return 8;
    }
    std::printf("applied attempt=%llu\n",
                static_cast<unsigned long long>(outcome.attempt.value()));
    if (arguments.kill_at == "after_apply_before_verify") {
      HardExit(73);
    }
    return 0;
  }

  if (arguments.mode == "scenario") {
    // Everything happens inside one process incarnation so that the recorded
    // observation is fresh evidence for the plan this same incarnation builds.
    // The chosen kill boundary then lands inside a genuinely in-flight
    // reconciliation.
    fr::IntentDocument document;
    const auto intent_id = fr::IntentId::TryParse("intent/worker-scenario");
    const auto definition = fr::DefinitionId::TryParse("definition/worker");
    const auto observer = fr::ReporterId::TryParse("reporter/worker");
    const auto observation_id = fr::ObservationId::TryParse("observation/worker/scenario");
    const auto key = fr::AttributeKey::TryParse("mtu");
    if (!intent_id.has_value() || !definition.has_value() || !observer.has_value() ||
        !observation_id.has_value() || !key.has_value()) {
      Die("invalid identity", 2);
    }
    document.intent = *intent_id;
    document.scope = *scope;
    document.definition = *definition;
    document.generation = fr::IntentGeneration(1);
    document.policy_version = fr::PolicyVersion(1);
    document.evidence = fr::EvidenceClass::Synthetic;
    document.complete = true;
    fr::ObservationSubmission submission;
    submission.observation = *observation_id;
    submission.scope = *scope;
    submission.reporter = *observer;
    submission.evidence = fr::EvidenceClass::Synthetic;
    submission.complete = true;
    submission.authoritative_absence = true;
    for (long index = 0; index < arguments.subjects; ++index) {
      const auto subject = fr::SubjectId::TryParse(build_subject(index));
      if (!subject.has_value()) {
        Die("invalid subject", 2);
      }
      fr::SubjectIntent declared;
      declared.id = *subject;
      declared.desired.emplace(*key, fr::AttributeValue::Unsigned(9000));
      document.subjects.emplace(*subject, std::move(declared));
      fr::SubjectObservation seen;
      seen.id = *subject;
      seen.observed.emplace(
          *key, fr::AttributeValue::Unsigned(((index % 3) == 0) ? 1500u : 9000u));
      submission.subjects.emplace(*subject, std::move(seen));
    }
    fr::IntentCommitResult commit_result;
    if (!engine->CommitIntent(document, commit_result).ok()) {
      Die("scenario intent commit failed", 4);
    }
    // A rerun publishes a new generation rather than republishing the old
    // identity, which the store would rightly refuse as contradictory evidence.
    fr::ObservationGeneration generation(1);
    const auto retained = engine->ObservationEvidence(*scope);
    if (retained.ok()) {
      for (const fr::ObservationDocument& retained_evidence : *retained) {
        if (!(retained_evidence.reporter == submission.reporter)) {
          continue;
        }
        const fr::ObservationGeneration next(retained_evidence.generation.value() + 1);
        if (generation < next) {
          generation = next;
        }
      }
    }
    submission.generation = generation;
    fr::ObservationCommitResult observation_result;
    if (!engine->RecordObservationAt(submission, arguments.now, observation_result).ok()) {
      Die("scenario observation failed", 5);
    }
    fr::PlanRequest request;
    request.scope = *scope;
    request.now_unix_ms = arguments.now;
    const auto plan = engine->Plan(request);
    if (!plan.ok()) {
      std::fprintf(stderr, "plan failed: %s\n", plan.status().ToString().c_str());
      return 6;
    }
    std::printf("plan id=%s verdict=%s actions=%zu\n", plan->plan_id.c_str(),
                fr::ToText(plan->verdict), plan->MutationCount());
    if (arguments.kill_at == "before_plan") {
      HardExit(69);
    }
    if (arguments.kill_at == "before_dispatch") {
      HardExit(70);
    }
    const auto dispatched = engine->Dispatch(*plan);
    if (!dispatched.ok()) {
      std::fprintf(stderr, "dispatch failed: %s\n", dispatched.status().ToString().c_str());
      return 7;
    }
    std::printf("dispatched=%zu\n", dispatched->intents.size());
    if (dispatched->intents.empty()) {
      return 0;
    }
    if (arguments.kill_at == "after_dispatch_before_ack") {
      HardExit(71);
    }
    fr::CompletionRequest completion;
    completion.attempt = dispatched->intents.front().attempt;
    completion.epoch = engine->epoch();
    completion.boot = engine->boot();
    completion.idempotency_key = dispatched->intents.front().idempotency_key;
    completion.target = fr::AttemptState::Applied;
    completion.at_unix_ms = arguments.now + 10;
    if (arguments.kill_at == "before_completion_commit") {
      HardExit(72);
    }
    fr::ReconciliationOutcome outcome;
    if (!engine->Complete(completion, outcome).ok()) {
      Die("scenario completion failed", 8);
    }
    std::printf("applied attempt=%llu\n",
                static_cast<unsigned long long>(outcome.attempt.value()));
    if (arguments.kill_at == "after_apply_before_verify") {
      HardExit(73);
    }
    return 0;
  }

  if (arguments.mode == "report") {
    std::printf("%s", fr::RenderBootReportText(engine->boot_report()).c_str());
    const auto attempts = engine->Attempts(*scope);
    if (attempts.ok()) {
      for (const fr::AttemptRecord& attempt : *attempts) {
        std::printf("%s", fr::RenderAttemptText(attempt).c_str());
      }
    }
    const auto outcomes = engine->Outcomes(*scope);
    if (outcomes.ok()) {
      for (const fr::ReconciliationOutcome& outcome : *outcomes) {
        std::printf("outcome attempt=%llu state=%s verified=%s ambiguous=%s\n",
                    static_cast<unsigned long long>(outcome.attempt.value()),
                    fr::ToText(outcome.terminal_state), outcome.verified ? "yes" : "no",
                    outcome.ambiguous ? "yes" : "no");
      }
    }
    return 0;
  }

  if (arguments.mode == "compact") {
    const fr::Status status = engine->Compact();
    if (!status.ok()) {
      std::fprintf(stderr, "compact failed: %s\n", status.ToString().c_str());
      return 9;
    }
    std::printf("compacted\n");
    return 0;
  }

  Die("unknown mode", 2);
}
