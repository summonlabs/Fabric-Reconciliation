// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Classification proofs: every drift class, the evidence selection rules, and
// the guarantee that a coverage gap is never reported as an absence.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

struct Fixture {
  fr::IntentDocument intent;
  fr::ObservationDocument observation;
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  std::vector<const fr::IntentDocument*> intents;
  std::vector<const fr::ObservationDocument*> observations;
  bool has_intent{true};
  fr::CoordinatorEpoch live_epoch{fr::CoordinatorEpoch(4)};
  fr::UnixMillis now{1000};

  fr::ClassificationReport Run() {
    intents.clear();
    observations.clear();
    if (has_intent) {
      intents.push_back(&intent);
    }
    observations.push_back(&observation);
    fr::ClassificationInput input;
    input.scope = intent.scope;
    input.intents = &intents;
    input.observations = &observations;
    input.has_committed_intent = has_intent;
    input.committed_intent_generation = intent.generation;
    input.policy = &policy;
    input.limits = nullptr;
    input.live_epoch = live_epoch;
    input.now_unix_ms = now;
    fr::ClassificationReport report;
    const fr::Status status = fr::ClassifyScope(input, report);
    if (!status.ok()) {
      throw std::runtime_error("classify failed: " + status.ToString());
    }
    return report;
  }
};

fr::ObservationDocument Stamp(const fr::ObservationSubmission& submission, fr::CoordinatorEpoch epoch,
                              fr::UnixMillis at) {
  fr::ObservationDocument document;
  document.observation = submission.observation;
  document.scope = submission.scope;
  document.reporter = submission.reporter;
  document.generation = submission.generation;
  document.evidence = submission.evidence;
  document.complete = submission.complete;
  document.authoritative_absence = submission.authoritative_absence;
  document.received_epoch = epoch;
  document.received_unix_ms = at;
  document.subjects = submission.subjects;
  document.digest = fr::ComputeObservationDigest(document);
  return document;
}

fr::IntentDocument SingleIntent(unsigned desired_mtu) {
  fr::IntentDocument document = MakeIntent("fabric/rack-7", 1, 1, fr::EvidenceClass::Synthetic);
  document.subjects.clear();
  const fr::SubjectId subject = RequireSubject("node/n0/port/eth0");
  fr::SubjectIntent entry;
  entry.id = subject;
  entry.desired.emplace(RequireAttribute("mtu"), fr::AttributeValue::Unsigned(desired_mtu));
  document.subjects.emplace(subject, std::move(entry));
  document.digest = fr::ComputeIntentDigest(document);
  return document;
}

fr::DriftClass ClassOf(const fr::ClassificationReport& report, const char* subject) {
  const fr::SubjectId id = RequireSubject(subject);
  for (const fr::DriftRecord& record : report.records) {
    if (record.subject == id) {
      return record.drift;
    }
  }
  throw std::runtime_error("subject not classified");
}

}  // namespace

FR_TEST(freshness, epoch_mismatch_is_stale_even_when_the_timestamp_is_recent) {
  const fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationDocument document = Stamp(submission, fr::CoordinatorEpoch(3), 1000);
  FR_CHECK(fr::EvaluateFreshness(document, fr::DefaultPolicy().freshness, fr::CoordinatorEpoch(4),
                                 1000) == fr::FreshnessVerdict::ForeignEpoch);
}

FR_TEST(freshness, expiry_and_clock_regression_are_stale) {
  const fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationDocument document = Stamp(submission, fr::CoordinatorEpoch(4), 1000);
  fr::FreshnessPolicy policy = fr::DefaultPolicy().freshness;
  policy.max_observation_age_ms = 500;
  FR_CHECK(fr::EvaluateFreshness(document, policy, fr::CoordinatorEpoch(4), 1400) ==
           fr::FreshnessVerdict::Fresh);
  FR_CHECK(fr::EvaluateFreshness(document, policy, fr::CoordinatorEpoch(4), 1501) ==
           fr::FreshnessVerdict::Expired);
  FR_CHECK(fr::EvaluateFreshness(document, policy, fr::CoordinatorEpoch(4), 999) ==
           fr::FreshnessVerdict::ClockRegression);
  // A foreign epoch is tolerated only when policy says so; the temporal rules
  // then still apply, and a clock that moved backwards is stale.
  policy.require_current_epoch = false;
  policy.max_observation_age_ms = 500;
  FR_CHECK(fr::EvaluateFreshness(document, policy, fr::CoordinatorEpoch(9), 999) ==
           fr::FreshnessVerdict::ClockRegression);
  policy.max_observation_age_ms = 0;
  FR_CHECK(fr::EvaluateFreshness(document, policy, fr::CoordinatorEpoch(9), 999) ==
           fr::FreshnessVerdict::Fresh);
}

FR_TEST(selection, highest_generation_wins_and_identical_duplicates_are_tolerated) {
  const fr::ObservationSubmission older =
      MakeObservation("fabric/rack-7", "reporter/a", 2, 2, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission newer =
      MakeObservation("fabric/rack-7", "reporter/a", 5, 2, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationDocument first = Stamp(older, fr::CoordinatorEpoch(4), 1000);
  const fr::ObservationDocument second = Stamp(newer, fr::CoordinatorEpoch(4), 1000);
  std::vector<const fr::ObservationDocument*> candidates{&first, &second};
  const fr::ObservationSelection selection = fr::SelectCurrentObservation(candidates);
  FR_CHECK(selection.selected == &second);
  FR_CHECK(!selection.conflicted);

  std::vector<const fr::ObservationDocument*> duplicates{&second, &second};
  const fr::ObservationSelection duplicate_selection = fr::SelectCurrentObservation(duplicates);
  FR_CHECK(duplicate_selection.selected == &second);
  FR_CHECK(!duplicate_selection.conflicted);
}

FR_TEST(selection, conflicting_publishers_at_one_generation_are_a_conflict) {
  const fr::ObservationSubmission left =
      MakeObservation("fabric/rack-7", "reporter/a", 4, 2, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission right =
      MakeObservation("fabric/rack-7", "reporter/b", 4, 2, 2, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationDocument first = Stamp(left, fr::CoordinatorEpoch(4), 1000);
  const fr::ObservationDocument second = Stamp(right, fr::CoordinatorEpoch(4), 1000);
  std::vector<const fr::ObservationDocument*> candidates{&first, &second};
  const fr::ObservationSelection selection = fr::SelectCurrentObservation(candidates);
  FR_CHECK(selection.selected == nullptr);
  FR_CHECK(selection.conflicted);
  FR_CHECK(selection.reason == fr::ReasonCode::ConflictingRepublishers);
}

FR_TEST(selection, intent_conflict_at_one_generation) {
  const fr::IntentDocument left = MakeIntent("fabric/rack-7", 4, 2, fr::EvidenceClass::Synthetic);
  fr::IntentDocument right = left;
  right.subjects.begin()->second.desired[RequireAttribute("mtu")] =
      fr::AttributeValue::Unsigned(1500);
  right.digest = fr::ComputeIntentDigest(right);
  std::vector<const fr::IntentDocument*> candidates{&left, &right};
  const fr::IntentSelection selection = fr::SelectCurrentIntent(candidates);
  FR_CHECK(selection.selected == nullptr);
  FR_CHECK(selection.conflicted);
}

FR_TEST(classification, already_converged) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 0, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK_EQ(report.records.size(), std::size_t(1));
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::AlreadyConverged);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::ClassifiedAlreadyConverged);
  FR_CHECK(report.records[0].deltas.empty());
}

FR_TEST(classification, missing_requires_complete_and_absence_authority) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 0, 5, false, fr::EvidenceClass::Synthetic);
  submission.complete = true;
  submission.authoritative_absence = true;
  fixture.observation = Stamp(submission, fr::CoordinatorEpoch(4), 1000);
  FR_CHECK(ClassOf(fixture.Run(), "node/n0/port/eth0") == fr::DriftClass::Missing);

  fr::ObservationSubmission no_absence = submission;
  no_absence.authoritative_absence = false;
  fixture.observation = Stamp(no_absence, fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport second = fixture.Run();
  FR_CHECK(ClassOf(second, "node/n0/port/eth0") == fr::DriftClass::Unknown);
  FR_CHECK_EQ(second.records[0].reason, fr::ReasonCode::ObservationCannotAssertAbsence);

  fr::ObservationSubmission incomplete = submission;
  incomplete.complete = false;
  fixture.observation = Stamp(incomplete, fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport third = fixture.Run();
  FR_CHECK(ClassOf(third, "node/n0/port/eth0") == fr::DriftClass::Unknown);
  FR_CHECK_EQ(third.records[0].reason, fr::ReasonCode::ObservationCoverageIncomplete);
}

FR_TEST(classification, unexpected_requires_a_complete_intent) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.intent.complete = true;
  fixture.intent.subjects.clear();
  fixture.intent.digest = fr::ComputeIntentDigest(fixture.intent);
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  FR_CHECK(ClassOf(fixture.Run(), "node/n0/port/eth0") == fr::DriftClass::Unexpected);

  fixture.intent.complete = false;
  fixture.intent.digest = fr::ComputeIntentDigest(fixture.intent);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Unknown);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::IntentCoverageIncomplete);
}

FR_TEST(classification, mismatched_attribute_is_reported_with_its_delta) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Mismatched);
  FR_CHECK_EQ(report.records[0].deltas.size(), std::size_t(1));
  FR_CHECK(report.records[0].deltas[0].kind == fr::AttributeDeltaKind::ValueMismatch);
  FR_CHECK_EQ(report.records[0].deltas[0].intended.Render(), std::string("9000"));
  FR_CHECK_EQ(report.records[0].deltas[0].observed.Render(), std::string("1500"));
}

FR_TEST(classification, attribute_coverage_gap_is_unknown_not_mismatch) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic);
  submission.subjects.begin()->second.observed.clear();
  submission.subjects.begin()->second.complete_attributes = false;
  fixture.observation = Stamp(submission, fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Unknown);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::AttributeCoverageIncomplete);

  submission.subjects.begin()->second.complete_attributes = true;
  fixture.observation = Stamp(submission, fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport second = fixture.Run();
  FR_CHECK(ClassOf(second, "node/n0/port/eth0") == fr::DriftClass::Mismatched);
  FR_CHECK(second.records[0].deltas[0].kind == fr::AttributeDeltaKind::OnlyInIntent);
}

FR_TEST(classification, not_comparable_attribute_kinds_are_not_coerced) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fr::ObservationSubmission submission =
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic);
  submission.subjects.begin()->second.observed[RequireAttribute("mtu")] =
      fr::AttributeValue::Text("9000");
  fixture.observation = Stamp(submission, fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Mismatched);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::ClassifiedAttributeNotComparable);
  FR_CHECK(report.records[0].deltas[0].kind == fr::AttributeDeltaKind::NotComparable);
}

FR_TEST(classification, stale_observation_marks_every_subject) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 5, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(1), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::StaleObservation);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::ObservationForeignEpoch);
  FR_CHECK(report.evidence.freshness == fr::FreshnessVerdict::ForeignEpoch);
}

FR_TEST(classification, no_observation_at_all_is_unknown) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.observations.clear();
  fr::ClassificationInput input;
  input.scope = fixture.intent.scope;
  std::vector<const fr::IntentDocument*> intents{&fixture.intent};
  input.intents = &intents;
  input.observations = nullptr;
  input.has_committed_intent = true;
  input.committed_intent_generation = fixture.intent.generation;
  input.policy = &fixture.policy;
  input.live_epoch = fixture.live_epoch;
  input.now_unix_ms = fixture.now;
  fr::ClassificationReport report;
  FR_CHECK(fr::ClassifyScope(input, report).ok());
  FR_CHECK_EQ(report.records.size(), std::size_t(1));
  FR_CHECK(report.records[0].drift == fr::DriftClass::Unknown);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::NoObservationForScope);
}

FR_TEST(classification, regressed_intent_generation_is_stale_intent) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  std::vector<const fr::IntentDocument*> intents{&fixture.intent};
  std::vector<const fr::ObservationDocument*> observations;
  fr::ClassificationInput input;
  input.scope = fixture.intent.scope;
  input.intents = &intents;
  input.observations = &observations;
  input.has_committed_intent = true;
  input.committed_intent_generation = fr::IntentGeneration(9);
  input.policy = &fixture.policy;
  input.live_epoch = fixture.live_epoch;
  input.now_unix_ms = fixture.now;
  fr::ClassificationReport report;
  FR_CHECK(fr::ClassifyScope(input, report).ok());
  FR_CHECK_EQ(report.records.size(), std::size_t(1));
  FR_CHECK(report.records[0].drift == fr::DriftClass::StaleIntent);
  FR_CHECK_EQ(report.records[0].reason, fr::ReasonCode::IntentGenerationRegressed);
}

FR_TEST(classification, conflicting_publishers_make_the_whole_scope_conflicted) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  const fr::ObservationSubmission left =
      MakeObservation("fabric/rack-7", "reporter/a", 3, 1, 5, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationSubmission right =
      MakeObservation("fabric/rack-7", "reporter/b", 3, 1, 1, false, fr::EvidenceClass::Synthetic);
  const fr::ObservationDocument first = Stamp(left, fr::CoordinatorEpoch(4), 1000);
  const fr::ObservationDocument second = Stamp(right, fr::CoordinatorEpoch(4), 1000);
  std::vector<const fr::IntentDocument*> intents{&fixture.intent};
  std::vector<const fr::ObservationDocument*> observations{&first, &second};
  fr::ClassificationInput input;
  input.scope = fixture.intent.scope;
  input.intents = &intents;
  input.observations = &observations;
  input.has_committed_intent = true;
  input.committed_intent_generation = fixture.intent.generation;
  input.policy = &fixture.policy;
  input.live_epoch = fixture.live_epoch;
  input.now_unix_ms = fixture.now;
  fr::ClassificationReport report;
  FR_CHECK(fr::ClassifyScope(input, report).ok());
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Conflict);
}

FR_TEST(classification, unmanaged_attributes_do_not_create_drift) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.policy.unmanaged_attributes = {RequireAttribute("mtu")};
  FR_CHECK(fr::CanonicalisePolicy(fixture.policy, fr::RuntimeLimits{}).ok());
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::AlreadyConverged);
  FR_CHECK_EQ(report.records[0].deltas.size(), std::size_t(1));
  FR_CHECK(report.records[0].deltas[0].kind == fr::AttributeDeltaKind::Unmanaged);
}

FR_TEST(classification, observe_only_attributes_are_reported_as_such) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fixture.policy.observe_only_attributes = {RequireAttribute("mtu")};
  FR_CHECK(fr::CanonicalisePolicy(fixture.policy, fr::RuntimeLimits{}).ok());
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(ClassOf(report, "node/n0/port/eth0") == fr::DriftClass::Mismatched);
  FR_CHECK(report.records[0].deltas[0].kind == fr::AttributeDeltaKind::ObserveOnly);
}

FR_TEST(classification, empty_intent_and_empty_observation_agree) {
  Fixture fixture;
  fixture.intent = MakeIntent("fabric/rack-7", 1, 0, fr::EvidenceClass::Synthetic);
  fixture.observation = Stamp(
      MakeObservation("fabric/rack-7", "reporter/a", 1, 0, 5, false, fr::EvidenceClass::Synthetic),
      fr::CoordinatorEpoch(4), 1000);
  const fr::ClassificationReport report = fixture.Run();
  FR_CHECK(report.records.empty());
  FR_CHECK_EQ(report.subject_visits, 0ull);
}

FR_TEST(classification, work_counters_are_linear_in_subjects) {
  const std::size_t sizes[] = {64, 256, 1024};
  std::uint64_t previous_visits = 0;
  for (const std::size_t size : sizes) {
    Fixture fixture;
    fixture.intent = MakeIntent("fabric/rack-7", 1, size, fr::EvidenceClass::Synthetic);
    fixture.observation = Stamp(
        MakeObservation("fabric/rack-7", "reporter/a", 1, size, 5, false,
                        fr::EvidenceClass::Synthetic),
        fr::CoordinatorEpoch(4), 1000);
    const fr::ClassificationReport report = fixture.Run();
    FR_CHECK_EQ(report.records.size(), size);
    FR_CHECK_EQ(report.subject_visits, static_cast<std::uint64_t>(size));
    if (previous_visits != 0) {
      // 4x the subjects must be 4x the visits, never 16x.
      FR_CHECK(report.subject_visits <= previous_visits * 5);
    }
    previous_visits = report.subject_visits;
  }
}

FR_TEST(classification, mismatched_scope_candidates_are_refused) {
  Fixture fixture;
  fixture.intent = SingleIntent(9000);
  fr::IntentDocument foreign = SingleIntent(9000);
  foreign.scope = RequireScope("fabric/rack-9");
  std::vector<const fr::IntentDocument*> intents{&fixture.intent, &foreign};
  fr::ClassificationInput input;
  input.scope = fixture.intent.scope;
  input.intents = &intents;
  input.observations = nullptr;
  input.policy = &fixture.policy;
  input.live_epoch = fixture.live_epoch;
  input.now_unix_ms = fixture.now;
  fr::ClassificationReport report;
  const fr::Status status = fr::ClassifyScope(input, report);
  FR_CHECK(!status.ok());
}

FR_TEST(classification, policy_is_required) {
  fr::ClassificationInput input;
  input.scope = RequireScope("fabric/rack-7");
  input.live_epoch = fr::CoordinatorEpoch(1);
  fr::ClassificationReport report;
  const fr::Status status = fr::ClassifyScope(input, report);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::PreconditionFailed);
}
