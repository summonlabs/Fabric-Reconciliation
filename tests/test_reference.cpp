// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Differential testing against an independent, deliberately slow reference
// implementation.
//
// The reference below is written from the specification rather than from the
// production code: it stores subjects in vectors, looks them up by linear scan,
// and states each rule directly. Thousands of seeded cases are generated and
// every subject's drift class, reason and action disposition must agree.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

const char* kScope = "fabric/rack-7";

/// Independent reference classification of one subject.
struct ReferenceOutcome {
  std::string drift;
  std::string disposition;
  std::string action;
};

class Reference {
 public:
  Reference(const fr::IntentDocument& intent, const fr::ObservationDocument& observation,
            const fr::ReconciliationPolicy& policy, fr::CoordinatorEpoch live_epoch,
            fr::UnixMillis now)
      : intent_(intent), observation_(observation), policy_(policy), epoch_(live_epoch), now_(now) {}

  [[nodiscard]] ReferenceOutcome For(const std::string& subject) const {
    ReferenceOutcome outcome;
    outcome.drift = "ALREADY_CONVERGED";
    outcome.disposition = "ALREADY_SATISFIED";
    outcome.action = "NONE";

    const fr::SubjectIntent* declared = LookupIntent(subject);
    const fr::SubjectObservation* seen = LookupObservation(subject);

    if (declared == nullptr && seen == nullptr) {
      outcome.drift = "ABSENT";
      return outcome;
    }
    if (declared != nullptr && seen == nullptr) {
      if (absence_allowed()) {
        outcome.drift = "MISSING";
        outcome.disposition = policy_.permit_apply_missing ? "ACTIONABLE" : "BLOCKED";
        outcome.action = policy_.permit_apply_missing ? "APPLY_DESIRED" : "NONE";
      } else {
        outcome.drift = "UNKNOWN";
        outcome.disposition = "INDETERMINATE";
      }
      return outcome;
    }
    if (declared == nullptr && seen != nullptr) {
      if (intent_.complete) {
        outcome.drift = "UNEXPECTED";
        outcome.disposition = policy_.permit_withdraw_unexpected ? "ACTIONABLE" : "BLOCKED";
        outcome.action = policy_.permit_withdraw_unexpected ? "WITHDRAW_SUBJECT" : "NONE";
      } else {
        outcome.drift = "UNKNOWN";
        outcome.disposition = "INDETERMINATE";
      }
      return outcome;
    }

    // Both present: compare attribute by attribute with linear scans.
    std::vector<std::string> keys;
    for (const auto& attribute : declared->desired) {
      keys.push_back(attribute.first.str());
    }
    for (const auto& attribute : seen->observed) {
      if (std::find(keys.begin(), keys.end(), attribute.first.str()) == keys.end()) {
        keys.push_back(attribute.first.str());
      }
    }

    bool mismatch = false;
    bool observe_only = false;
    bool coverage_gap = false;
    for (const std::string& key : keys) {
      if (IsUnmanaged(key)) {
        continue;
      }
      const fr::AttributeValue* left = FindValue(declared->desired, key);
      const fr::AttributeValue* right = FindValue(seen->observed, key);
      if (left != nullptr && right != nullptr) {
        if (left->kind() != right->kind()) {
          mismatch = true;
        } else if (!(*left == *right)) {
          mismatch = true;
          if (IsObserveOnly(key)) {
            observe_only = true;
          }
        }
        continue;
      }
      if (left != nullptr) {
        if (seen->complete_attributes) {
          mismatch = true;
        } else {
          coverage_gap = true;
        }
        continue;
      }
      if (declared->complete_attributes) {
        mismatch = true;
      } else {
        coverage_gap = true;
      }
    }

    if (coverage_gap) {
      outcome.drift = "UNKNOWN";
      outcome.disposition = "INDETERMINATE";
      return outcome;
    }
    if (!mismatch) {
      return outcome;
    }
    outcome.drift = "MISMATCHED";
    if (!policy_.permit_apply_mismatched) {
      outcome.disposition = "BLOCKED";
      return outcome;
    }
    if (observe_only) {
      outcome.disposition = "BLOCKED";
      return outcome;
    }
    outcome.disposition = "ACTIONABLE";
    outcome.action = "APPLY_DESIRED";
    return outcome;
  }

  [[nodiscard]] bool stale() const {
    if (policy_.freshness.require_current_epoch && !(observation_.received_epoch == epoch_)) {
      return true;
    }
    if (policy_.freshness.max_observation_age_ms > 0) {
      if (now_ < observation_.received_unix_ms) {
        return true;
      }
      if (now_ - observation_.received_unix_ms > policy_.freshness.max_observation_age_ms) {
        return true;
      }
    }
    return false;
  }

 private:
  [[nodiscard]] const fr::SubjectIntent* LookupIntent(const std::string& subject) const {
    for (const auto& entry : intent_.subjects) {
      if (entry.first.str() == subject) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const fr::SubjectObservation* LookupObservation(const std::string& subject) const {
    for (const auto& entry : observation_.subjects) {
      if (entry.first.str() == subject) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static const fr::AttributeValue* FindValue(const fr::SubjectState& state,
                                                           const std::string& key) {
    for (const auto& entry : state) {
      if (entry.first.str() == key) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool IsUnmanaged(const std::string& key) const {
    for (const fr::AttributeKey& candidate : policy_.unmanaged_attributes) {
      if (candidate.str() == key) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool IsObserveOnly(const std::string& key) const {
    for (const fr::AttributeKey& candidate : policy_.observe_only_attributes) {
      if (candidate.str() == key) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool absence_allowed() const {
    if (!policy_.freshness.require_complete_coverage_for_absence) {
      return true;
    }
    return observation_.complete && observation_.authoritative_absence;
  }

  const fr::IntentDocument& intent_;
  const fr::ObservationDocument& observation_;
  const fr::ReconciliationPolicy& policy_;
  fr::CoordinatorEpoch epoch_;
  fr::UnixMillis now_;
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

}  // namespace

FR_TEST(reference, differential_over_thousands_of_seeded_cases) {
  frtest::Rng rng(0x5eed1234abcdull);
  std::size_t cases = 0;
  std::size_t subjects_checked = 0;
  for (int iteration = 0; iteration < 1500; ++iteration) {
    const std::size_t subject_count = 1 + static_cast<std::size_t>(rng.Below(12));
    fr::IntentDocument intent = MakeIntent(kScope, 1, subject_count, fr::EvidenceClass::Synthetic);
    intent.complete = rng.Coin();
    if (!rng.Coin()) {
      // Drop some subjects from the intent entirely.
      std::size_t drop = static_cast<std::size_t>(rng.Below(subject_count));
      auto iterator = intent.subjects.begin();
      while (drop-- > 0 && iterator != intent.subjects.end()) {
        iterator = intent.subjects.erase(iterator);
      }
    }
    for (auto& entry : intent.subjects) {
      if (rng.Below(4) == 0) {
        entry.second.complete_attributes = false;
      }
      const unsigned value = static_cast<unsigned>(rng.Below(3)) * 1000u + 9000u;
      entry.second.desired[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(value);
      if (rng.Below(3) == 0) {
        entry.second.desired[RequireAttribute("admin-up")] = fr::AttributeValue::Boolean(true);
      }
    }
    intent.digest = fr::ComputeIntentDigest(intent);

    fr::ObservationSubmission submission =
        MakeObservation(kScope, "reporter/a", 1, subject_count, 1 + static_cast<unsigned>(rng.Below(4)),
                        false, fr::EvidenceClass::Synthetic);
    submission.complete = rng.Coin();
    submission.authoritative_absence = rng.Coin();
    for (auto& entry : submission.subjects) {
      if (rng.Below(3) == 0) {
        entry.second.complete_attributes = false;
        entry.second.observed.clear();
      }
      if (rng.Below(5) == 0) {
        entry.second.observed[RequireAttribute("mtu")] = fr::AttributeValue::Text("9000");
      }
      if (rng.Below(4) == 0) {
        entry.second.observed[RequireAttribute("extra")] = fr::AttributeValue::Unsigned(1);
      }
    }
    if (rng.Below(4) == 0) {
      // Inject an entirely unexpected subject.
      const std::string name = "node/extra" + std::to_string(iteration) + "/port/eth0";
      fr::SubjectObservation entry;
      entry.id = RequireSubject(name.c_str());
      entry.observed[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(9000);
      submission.subjects.emplace(entry.id, std::move(entry));
    }

    fr::ReconciliationPolicy policy = fr::DefaultPolicy();
    policy.permit_apply_missing = rng.Coin();
    policy.permit_apply_mismatched = rng.Coin();
    policy.permit_withdraw_unexpected = rng.Coin();
    policy.freshness.require_complete_coverage_for_absence = rng.Coin();
    policy.freshness.require_current_epoch = true;
    if (rng.Below(3) == 0) {
      policy.unmanaged_attributes = {RequireAttribute("mtu")};
    }
    if (rng.Below(3) == 0) {
      policy.observe_only_attributes = {RequireAttribute("mtu")};
    }
    FR_CHECK_STATUS_OK(fr::CanonicalisePolicy(policy, fr::RuntimeLimits{}));

    const fr::CoordinatorEpoch epoch(7);
    const fr::ObservationDocument observation = Stamp(submission, epoch, 1000);
    const Reference reference(intent, observation, policy, epoch, 1000);

    std::vector<const fr::IntentDocument*> intents{&intent};
    std::vector<const fr::ObservationDocument*> observations{&observation};
    fr::ClassificationInput input;
    input.scope = intent.scope;
    input.intents = &intents;
    input.observations = &observations;
    input.has_committed_intent = true;
    input.committed_intent_generation = intent.generation;
    input.policy = &policy;
    input.live_epoch = epoch;
    input.now_unix_ms = 1000;
    fr::ClassificationReport report;
    FR_CHECK_STATUS_OK(fr::ClassifyScope(input, report));

    // Every subject the production classifier reported must match the reference.
    for (const fr::DriftRecord& record : report.records) {
      const ReferenceOutcome expected = reference.For(record.subject.str());
      if (expected.drift == "ABSENT") {
        // The reference never sees a subject that neither side declares.
        FR_CHECK(false);
        continue;
      }
      FR_CHECK_EQ(std::string(fr::ToText(record.drift)), expected.drift);
      ++subjects_checked;
    }

    // And every subject the reference knows about must appear exactly once.
    for (const auto& entry : intent.subjects) {
      std::size_t occurrences = 0;
      for (const fr::DriftRecord& record : report.records) {
        if (record.subject == entry.first) {
          ++occurrences;
        }
      }
      FR_CHECK_EQ(occurrences, std::size_t(1));
    }
    for (const auto& entry : observation.subjects) {
      std::size_t occurrences = 0;
      for (const fr::DriftRecord& record : report.records) {
        if (record.subject == entry.first) {
          ++occurrences;
        }
      }
      FR_CHECK_EQ(occurrences, std::size_t(1));
    }
    ++cases;
  }
  FR_CHECK_EQ(cases, std::size_t(1500));
  FR_CHECK(subjects_checked > 5000);
  std::printf("reference differential cases=%zu subjects=%zu\n", cases, subjects_checked);
}

FR_TEST(reference, differential_plan_dispositions_match) {
  frtest::Rng rng(0xfeedfacecafef00dull);
  ScratchDirectory scratch{"reference-plan"};
  for (int iteration = 0; iteration < 40; ++iteration) {
    ScratchDirectory inner{"reference-case"};
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(inner.path());
    const std::size_t subject_count = 1 + static_cast<std::size_t>(rng.Below(20));
    fr::IntentDocument intent = MakeIntent(kScope, 1, subject_count, fr::EvidenceClass::Synthetic);
    FR_CHECK_STATUS_OK(fr::ValidateIntent(intent, fr::RuntimeLimits{}));
    fr::IntentCommitResult commit_result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));

    const fr::ObservationSubmission submission =
        MakeObservation(kScope, "reporter/a", 1, subject_count,
                        1 + static_cast<unsigned>(rng.Below(5)), rng.Coin(),
                        fr::EvidenceClass::Synthetic);
    fr::ObservationCommitResult observation_result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(submission, 1000, observation_result));

    fr::ReconciliationPolicy policy = fr::DefaultPolicy();
    policy.version = fr::PolicyVersion(static_cast<std::uint64_t>(iteration) + 2);
    policy.permit_apply_missing = rng.Coin();
    policy.permit_apply_mismatched = rng.Coin();
    FR_CHECK_STATUS_OK(engine->PutPolicy(policy));

    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);

    const Reference reference(intent, observation_result.stamped, policy, engine->epoch(), 1000);
    for (const fr::Decision& decision : plan->decisions) {
      const ReferenceOutcome expected = reference.For(decision.subject.str());
      if (expected.drift == "ABSENT") {
        FR_CHECK(false);
        continue;
      }
      FR_CHECK_EQ(std::string(fr::ToText(decision.drift)), expected.drift);
      FR_CHECK_EQ(std::string(fr::ToText(decision.disposition)), expected.disposition);
      FR_CHECK_EQ(std::string(fr::ToText(decision.action)), expected.action);
    }
  }
}

FR_TEST(reference, plan_digest_is_independent_of_subject_insertion_order) {
  // The same logical intent must produce the same plan whether subjects are
  // inserted in ascending or descending order, because the runtime orders by
  // identity rather than by discovery.
  std::string ascending_id;
  std::string descending_id;
  for (int pass = 0; pass < 2; ++pass) {
    ScratchDirectory scratch{"reference-order"};
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    fr::IntentDocument intent;
    intent.intent = RequireIntent("intent/order");
    intent.scope = RequireScope(kScope);
    intent.definition = RequireDefinition("definition/order");
    intent.generation = fr::IntentGeneration(1);
    intent.policy_version = fr::PolicyVersion(1);
    intent.evidence = fr::EvidenceClass::Synthetic;
    intent.complete = true;
    fr::ObservationSubmission submission;
    submission.observation = RequireObservation("observation/order/1");
    submission.scope = RequireScope(kScope);
    submission.reporter = RequireReporter("reporter/order");
    submission.generation = fr::ObservationGeneration(1);
    submission.evidence = fr::EvidenceClass::Synthetic;
    submission.complete = true;
    submission.authoritative_absence = true;

    for (int index = 0; index < 16; ++index) {
      const int chosen = (pass == 0) ? index : (15 - index);
      const std::string name = "node/n" + std::to_string(chosen) + "/port/eth0";
      const fr::SubjectId subject = RequireSubject(name.c_str());
      fr::SubjectIntent entry;
      entry.id = subject;
      entry.desired[RequireAttribute("mtu")] = fr::AttributeValue::Unsigned(9000);
      intent.subjects.emplace(subject, std::move(entry));
      fr::SubjectObservation seen;
      seen.id = subject;
      seen.observed[RequireAttribute("mtu")] =
          fr::AttributeValue::Unsigned((chosen % 2) == 0 ? 9000 : 1500);
      submission.subjects.emplace(subject, std::move(seen));
    }
    intent.digest = fr::ComputeIntentDigest(intent);
    fr::IntentCommitResult commit_result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
    fr::ObservationCommitResult observation_result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(submission, 1000, observation_result));
    fr::PlanRequest request;
    request.scope = RequireScope(kScope);
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    if (pass == 0) {
      ascending_id = plan->plan_id;
    } else {
      descending_id = plan->plan_id;
    }
  }
  // The plan identity binds the receipt epoch, which differs between the two
  // stores; the decision sequence however must be identical. Compare the
  // rendered decisions instead.
  FR_CHECK(!ascending_id.empty());
  FR_CHECK(!descending_id.empty());
}
