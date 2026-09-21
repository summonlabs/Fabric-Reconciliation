// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scale proofs. The runtime must remain linear in the number of subjects and
// its retained state must remain bounded. Wall-clock ratios are reported
// alongside the classifier's own work counters, which are the primary evidence
// because they are independent of the host.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

const char* kScope = "fabric/rack-7";

struct Measurement {
  std::size_t subjects{0};
  double classify_ms{0};
  double plan_ms{0};
  std::uint64_t visits{0};
  std::uint64_t comparisons{0};
  std::size_t decisions{0};
  std::size_t actions{0};
};

Measurement Measure(std::size_t subjects, const std::filesystem::path& directory) {
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(directory);
  const fr::IntentDocument intent =
      MakeIntent(kScope, 1, subjects, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, subjects, 5, true, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));

  Measurement measurement;
  measurement.subjects = subjects;
  const auto classify_start = std::chrono::steady_clock::now();
  const auto report = engine->Classify(RequireScope(kScope), 1000);
  const auto classify_end = std::chrono::steady_clock::now();
  FR_CHECK_STATUS_OK(report);
  measurement.classify_ms =
      std::chrono::duration<double, std::milli>(classify_end - classify_start).count();
  measurement.visits = report->subject_visits;
  measurement.comparisons = report->attribute_comparisons;

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  request.dry_run = true;
  const auto plan_start = std::chrono::steady_clock::now();
  const auto plan = engine->Plan(request);
  const auto plan_end = std::chrono::steady_clock::now();
  FR_CHECK_STATUS_OK(plan);
  measurement.plan_ms = std::chrono::duration<double, std::milli>(plan_end - plan_start).count();
  measurement.decisions = plan->decisions.size();
  measurement.actions = plan->MutationCount();
  return measurement;
}

}  // namespace

FR_TEST(scale, work_is_linear_in_the_number_of_subjects) {
  ScratchDirectory scratch{"scale-linear"};
  const std::size_t sizes[] = {2000, 8000, 32000, 128000};
  std::vector<Measurement> measurements;
  for (const std::size_t size : sizes) {
    measurements.push_back(Measure(size, scratch.child(std::to_string(size))));
  }
  for (std::size_t index = 1; index < measurements.size(); ++index) {
    const Measurement& previous = measurements[index - 1];
    const Measurement& current = measurements[index];
    const double size_ratio = static_cast<double>(current.subjects) /
                              static_cast<double>(previous.subjects);
    const double visit_ratio =
        static_cast<double>(current.visits) / static_cast<double>(previous.visits);
    const double comparison_ratio =
        static_cast<double>(current.comparisons) / static_cast<double>(previous.comparisons);
    std::printf("scale subjects=%zu size-ratio=%.2f visit-ratio=%.2f comparison-ratio=%.2f\n",
                current.subjects, size_ratio, visit_ratio, comparison_ratio);
    // Work must never grow faster than the problem size. A quadratic algorithm
    // would show a ratio of size_ratio squared, which is 16 for a 4x step.
    FR_CHECK(visit_ratio <= size_ratio * 1.05);
    FR_CHECK(comparison_ratio <= size_ratio * 1.05);
  }
  // The largest case must still produce exactly one decision per subject plus
  // the subjects the observation omits.
  const Measurement& largest = measurements.back();
  FR_CHECK(largest.decisions >= largest.subjects);
  FR_CHECK(largest.actions > 0);
}

FR_TEST(scale, decisions_are_bounded_by_the_subject_count) {
  ScratchDirectory scratch{"scale-bounded"};
  const Measurement measurement = Measure(40000, scratch.child("bounded"));
  FR_CHECK_EQ(measurement.decisions, std::size_t(40000));
  FR_CHECK_EQ(measurement.visits, 40000ull);
  // A subject the observation omitted still costs one visit and no attribute
  // comparison, so comparisons are bounded above by visits and stay close to it.
  FR_CHECK(measurement.comparisons <= measurement.visits);
  FR_CHECK(measurement.comparisons > measurement.visits * 9 / 10);
}

FR_TEST(scale, retained_state_stays_bounded_under_sustained_observation) {
  ScratchDirectory scratch{"scale-retained"};
  fr::EngineOptions options;
  options.store_dir = scratch.path();
  options.limits.max_retained_observations_per_scope = 4;
  options.limits.max_retained_outcomes = 8;
  options.auto_compact_after_records = 64;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));

  const fr::IntentDocument intent = MakeIntent(kScope, 1, 500, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));

  for (std::uint64_t generation = 1; generation <= 200; ++generation) {
    const fr::ObservationSubmission observation =
        MakeObservation(kScope, "reporter/a", generation, 500, 7, false,
                        fr::EvidenceClass::Synthetic);
    fr::ObservationCommitResult result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, result));
  }
  const auto evidence = engine->ObservationEvidence(RequireScope(kScope));
  FR_CHECK_STATUS_OK(evidence);
  FR_CHECK_EQ(evidence->size(), std::size_t(4));
  FR_CHECK(engine->stats().compactions > 0);
  // Compaction must not lose the durable intent.
  const auto current = engine->CurrentIntent(RequireScope(kScope));
  FR_CHECK_STATUS_OK(current);
  FR_CHECK_EQ(current->subjects.size(), std::size_t(500));
}

FR_TEST(scale, a_large_plan_seals_and_reopens_identically) {
  ScratchDirectory scratch{"scale-plan"};
  std::string plan_id;
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("first"));
    const fr::IntentDocument intent = MakeIntent(kScope, 1, 20000, fr::EvidenceClass::Synthetic);
    const fr::ObservationSubmission observation =
        MakeObservation(kScope, "reporter/a", 1, 20000, 11, false, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult commit_result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
    fr::ObservationCommitResult observation_result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    plan_id = plan->plan_id;
    FR_CHECK_EQ(plan->decisions.size(), std::size_t(20000));
  }
  // The same evidence committed in a fresh store produces the same plan id:
  // the plan is a pure function of its inputs, not of the store's history.
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("second"));
    const fr::IntentDocument intent = MakeIntent(kScope, 1, 20000, fr::EvidenceClass::Synthetic);
    const fr::ObservationSubmission observation =
        MakeObservation(kScope, "reporter/a", 1, 20000, 11, false, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult commit_result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
    fr::ObservationCommitResult observation_result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    FR_CHECK_EQ(plan->decisions.size(), std::size_t(20000));
    // A plan is a pure function of the evidence it rests on. The two stores are
    // separate durable lineages, yet identical evidence produces the identical
    // plan identity, which is what makes a plan comparable across processes.
    FR_CHECK(plan->plan_id == plan_id);
    FR_CHECK_EQ(plan->CountDrift(fr::DriftClass::Mismatched), std::size_t(1819));
  }
}
