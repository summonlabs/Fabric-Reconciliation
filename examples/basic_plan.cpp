// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Smallest complete use of the public API: declare intent, record a synthetic
// observation, plan, and explain. Everything here is labelled SYNTHETIC because
// it runs against in-process fixtures rather than physical fabric elements.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "summon/fabric_reconciliation/document_json.hpp"
#include "summon/fabric_reconciliation/engine.hpp"

namespace fr = summon::fabric_reconciliation;

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      (argc > 1) ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / "fabric-reconciliation-example";
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  fr::EngineOptions options;
  options.store_dir = directory;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = fr::ReconciliationEngine::Open(options, engine);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.ToString().c_str());
    return 1;
  }

  const auto scope = fr::ScopeId::TryParse("fabric/rack-7");
  const auto subject = fr::SubjectId::TryParse("node/n2/port/eth0");
  const auto key = fr::AttributeKey::TryParse("mtu");
  if (!scope.has_value() || !subject.has_value() || !key.has_value()) {
    return 1;
  }

  fr::IntentDocument intent;
  intent.intent = *fr::IntentId::TryParse("intent/rack-7");
  intent.scope = *scope;
  intent.definition = *fr::DefinitionId::TryParse("definition/rack-7");
  intent.generation = fr::IntentGeneration(1);
  intent.policy_version = fr::PolicyVersion(1);
  intent.evidence = fr::EvidenceClass::Synthetic;
  intent.complete = true;
  fr::SubjectIntent declared;
  declared.id = *subject;
  declared.desired.emplace(*key, fr::AttributeValue::Unsigned(9000));
  intent.subjects.emplace(*subject, std::move(declared));

  fr::IntentCommitResult intent_result;
  if (!engine->CommitIntent(intent, intent_result).ok()) {
    return 1;
  }

  fr::ObservationSubmission observation;
  observation.observation = *fr::ObservationId::TryParse("observation/rack-7/1");
  observation.scope = *scope;
  observation.reporter = *fr::ReporterId::TryParse("reporter/probe");
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
    return 1;
  }

  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = 1000;
  request.dry_run = true;
  const auto plan = engine->Plan(request);
  if (!plan.ok()) {
    std::fprintf(stderr, "plan failed: %s\n", plan.status().ToString().c_str());
    return 1;
  }
  std::printf("%s", fr::RenderPlanText(*plan).c_str());
  std::printf("example plan-id=%s actions=%zu\n", plan->plan_id.c_str(), plan->MutationCount());

  std::filesystem::remove_all(directory, error);
  return 0;
}
