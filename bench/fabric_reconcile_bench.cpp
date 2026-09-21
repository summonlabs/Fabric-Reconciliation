// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scale benchmark.
//
// The benchmark measures completed work, not submission latency: each size is
// classified and planned to completion and the reported number is the elapsed
// time of that completed work plus the classifier's own work counters. The
// counters are the primary evidence against accidental quadratic behaviour;
// wall-clock ratios are reported alongside them.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/document_json.hpp"
#include "summon/fabric_reconciliation/engine.hpp"

namespace fr = summon::fabric_reconciliation;

namespace {

struct Result {
  std::size_t subjects{0};
  double classify_ms{0};
  double plan_ms{0};
  std::uint64_t subject_visits{0};
  std::uint64_t attribute_comparisons{0};
  std::size_t decisions{0};
  std::size_t actions{0};
  std::string plan_id;
};

Result RunCase(std::size_t subjects, const std::filesystem::path& root, unsigned mismatch_modulus) {
  const std::filesystem::path directory = root / ("case-" + std::to_string(subjects));
  std::error_code error;
  std::filesystem::remove_all(directory, error);

  fr::EngineOptions options;
  options.store_dir = directory;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = fr::ReconciliationEngine::Open(options, engine);
  if (!opened.ok()) {
    std::fprintf(stderr, "open failed: %s\n", opened.ToString().c_str());
    std::exit(2);
  }
  const auto scope = fr::ScopeId::TryParse("fabric/bench");
  const auto key = fr::AttributeKey::TryParse("mtu");
  if (!scope.has_value() || !key.has_value()) {
    std::exit(2);
  }

  fr::IntentDocument intent;
  intent.intent = *fr::IntentId::TryParse("intent/bench");
  intent.scope = *scope;
  intent.definition = *fr::DefinitionId::TryParse("definition/bench");
  intent.generation = fr::IntentGeneration(1);
  intent.policy_version = fr::PolicyVersion(1);
  intent.evidence = fr::EvidenceClass::Synthetic;
  intent.complete = true;

  fr::ObservationSubmission observation;
  observation.observation = *fr::ObservationId::TryParse("observation/bench/1");
  observation.scope = *scope;
  observation.reporter = *fr::ReporterId::TryParse("reporter/bench");
  observation.generation = fr::ObservationGeneration(1);
  observation.evidence = fr::EvidenceClass::Synthetic;
  observation.complete = true;
  observation.authoritative_absence = true;

  for (std::size_t index = 0; index < subjects; ++index) {
    const std::string name = "node/n" + std::to_string(index) + "/port/eth0";
    const auto subject = fr::SubjectId::TryParse(name);
    if (!subject.has_value()) {
      std::exit(2);
    }
    fr::SubjectIntent declared;
    declared.id = *subject;
    declared.desired.emplace(*key, fr::AttributeValue::Unsigned(9000));
    intent.subjects.emplace(*subject, std::move(declared));
    if ((index % 17) == 3) {
      continue;  // deliberately missing from the observation
    }
    fr::SubjectObservation seen;
    seen.id = *subject;
    const unsigned value = ((index % mismatch_modulus) == 0) ? 1500u : 9000u;
    seen.observed.emplace(*key, fr::AttributeValue::Unsigned(value));
    observation.subjects.emplace(*subject, std::move(seen));
  }

  fr::IntentCommitResult commit_result;
  const fr::Status committed = engine->CommitIntent(intent, commit_result);
  if (!committed.ok()) {
    std::fprintf(stderr, "commit failed: %s\n", committed.ToString().c_str());
    std::exit(3);
  }
  fr::ObservationCommitResult observation_result;
  const fr::Status recorded = engine->RecordObservationAt(observation, 1000, observation_result);
  if (!recorded.ok()) {
    std::fprintf(stderr, "observe failed: %s\n", recorded.ToString().c_str());
    std::exit(4);
  }

  Result result;
  result.subjects = subjects;
  const auto classify_start = std::chrono::steady_clock::now();
  const auto report = engine->Classify(*scope, 1000);
  const auto classify_end = std::chrono::steady_clock::now();
  if (!report.ok()) {
    std::fprintf(stderr, "classify failed: %s\n", report.status().ToString().c_str());
    std::exit(5);
  }
  result.classify_ms =
      std::chrono::duration<double, std::milli>(classify_end - classify_start).count();
  result.subject_visits = report->subject_visits;
  result.attribute_comparisons = report->attribute_comparisons;

  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = 1000;
  request.dry_run = true;
  const auto plan_start = std::chrono::steady_clock::now();
  const auto plan = engine->Plan(request);
  const auto plan_end = std::chrono::steady_clock::now();
  if (!plan.ok()) {
    std::fprintf(stderr, "plan failed: %s\n", plan.status().ToString().c_str());
    std::exit(6);
  }
  result.plan_ms = std::chrono::duration<double, std::milli>(plan_end - plan_start).count();
  result.decisions = plan->decisions.size();
  result.actions = plan->MutationCount();
  result.plan_id = plan->plan_id;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root =
      (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "fr-bench";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  const std::size_t sizes[] = {2000, 8000, 32000, 128000};
  std::vector<Result> results;
  std::printf("%10s %12s %12s %14s %14s %10s %10s %8s\n", "subjects", "classify_ms", "plan_ms",
              "subject_visits", "attr_compare", "decisions", "actions", "ratio");
  double previous = 0;
  std::size_t previous_size = 0;
  for (const std::size_t size : sizes) {
    const Result result = RunCase(size, root, 5);
    const double growth = (previous > 0) ? (result.classify_ms + result.plan_ms) / previous : 0.0;
    std::printf("%10zu %12.3f %12.3f %14llu %14llu %10zu %10zu %8.2f\n", result.subjects,
                result.classify_ms, result.plan_ms,
                static_cast<unsigned long long>(result.subject_visits),
                static_cast<unsigned long long>(result.attribute_comparisons),
                result.decisions, result.actions, growth);
    if (previous_size != 0) {
      const double size_ratio = static_cast<double>(result.subjects) /
                                static_cast<double>(previous_size);
      const double work_ratio =
          static_cast<double>(result.subject_visits) /
          static_cast<double>(results.back().subject_visits == 0 ? 1 : results.back().subject_visits);
      std::printf("           size x%.2f  subject-visit work x%.2f  declared-linear=%s\n",
                  size_ratio, work_ratio,
                  (work_ratio <= size_ratio * 1.05) ? "yes" : "NO");
    }
    previous = result.classify_ms + result.plan_ms;
    previous_size = result.subjects;
    results.push_back(result);
  }
  std::printf("plan-ids:\n");
  for (const Result& result : results) {
    std::printf("  %8zu %s\n", result.subjects, result.plan_id.c_str());
  }
  std::filesystem::remove_all(root, error);
  return 0;
}
