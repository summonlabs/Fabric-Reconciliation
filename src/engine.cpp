// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/engine.hpp"

#include <algorithm>
#include <sstream>

namespace summon {
namespace fabric_reconciliation {
namespace {

constexpr std::size_t kExplanationSteps = 12;

bool TransitionLegal(AttemptState from, AttemptState to) noexcept {
  switch (from) {
    case AttemptState::Issued:
      return to == AttemptState::Dispatched || to == AttemptState::Acknowledged ||
             to == AttemptState::Applied || to == AttemptState::Failed ||
             to == AttemptState::Abandoned || to == AttemptState::Superseded ||
             to == AttemptState::Fenced || to == AttemptState::Interrupted;
    case AttemptState::Dispatched:
      return to == AttemptState::Acknowledged || to == AttemptState::Applied ||
             to == AttemptState::Failed || to == AttemptState::Abandoned ||
             to == AttemptState::Superseded || to == AttemptState::Fenced ||
             to == AttemptState::Interrupted;
    case AttemptState::Acknowledged:
      return to == AttemptState::Applied || to == AttemptState::Failed ||
             to == AttemptState::Abandoned || to == AttemptState::Superseded ||
             to == AttemptState::Fenced || to == AttemptState::Interrupted;
    case AttemptState::Applied:
      return to == AttemptState::Verified || to == AttemptState::Failed ||
             to == AttemptState::Fenced || to == AttemptState::Interrupted;
    case AttemptState::Invalid:
    case AttemptState::Verified:
    case AttemptState::Failed:
    case AttemptState::Abandoned:
    case AttemptState::Superseded:
    case AttemptState::Fenced:
    case AttemptState::Interrupted:
      return false;
  }
  return false;
}

std::string SubjectKey(const ScopeId& scope, const SubjectId& subject) {
  std::string key = scope.str();
  key += '|';
  key += subject.str();
  return key;
}

}  // namespace

ReconciliationEngine::~ReconciliationEngine() = default;

Status ReconciliationEngine::Open(const EngineOptions& options,
                                  std::unique_ptr<ReconciliationEngine>& out) {
  if (options.store_dir.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "engine requires a store directory");
  }
  auto engine = std::unique_ptr<ReconciliationEngine>(new ReconciliationEngine());
  const Status status = engine->Boot(options);
  if (!status.ok()) {
    return status;
  }
  out = std::move(engine);
  return Status::Ok();
}

Status ReconciliationEngine::Boot(const EngineOptions& options) {
  options_ = options;
  StoreOpenReport store_report;
  auto store = std::make_unique<DurableStore>();
  const Status opened = DurableStore::Open(options.store_dir, options.limits,
                                           options.create_if_missing, options.read_only, *store,
                                           store_report);
  if (!opened.ok()) {
    return opened;
  }
  store_ = std::move(store);

  boot_report_ = BootReport{};
  boot_report_.store = store_->state().store;
  boot_report_.store_created = store_report.created;
  boot_report_.snapshot_loaded = store_report.snapshot_loaded;
  boot_report_.recovered_torn_tail = store_report.recovered_torn_tail;
  boot_report_.recovered_bytes = store_report.recovered_bytes;
  boot_report_.records_replayed = store_report.records_replayed;
  boot_report_.attempts_restored = store_report.attempts_restored;
  boot_report_.previous_epoch = store_report.last_epoch;
  for (const ReasonCode note : store_report.notes) {
    boot_report_.notes.push_back(note);
  }

  live_boot_ = BootId::Generate();

  if (options.read_only) {
    live_epoch_ = store_->state().last_epoch;
    boot_report_.epoch = live_epoch_;
    boot_report_.boot = live_boot_;
    boot_report_.observations_retained = 0;
    boot_report_.outcomes_retained = store_->state().outcomes.size();
    boot_report_.fences_active = store_->state().fences.size();
    boot_report_.notes.push_back(ReasonCode::RestartFreshnessNotRestored);
    boot_report_.notes.push_back(ReasonCode::RestartLeasesNotRestored);
    compaction_watermark_ = store_->state().sequence;
    return Status::Ok();
  }

  if (!store_->state().has_policy) {
    if (!options.install_initial_policy) {
      return Status(StatusCode::PreconditionFailed, ReasonCode::NoIntentForScope,
                    "store has no policy and no initial policy was supplied");
    }
    ReconciliationPolicy policy = options.initial_policy;
    if (policy.id.empty()) {
      policy = DefaultPolicy();
    }
    const Status canonical = CanonicalisePolicy(policy, options.limits);
    if (!canonical.ok()) {
      return canonical;
    }
    const Status installed = store_->PutPolicy(policy);
    if (!installed.ok()) {
      return installed;
    }
  }

  // A restart always creates a fresh coordinator epoch. Every quantity stamped
  // with the previous epoch is fenced by this advance.
  const CoordinatorEpoch previous = store_->state().last_epoch;
  const CoordinatorEpoch next(previous.value() + 1);
  const Status advanced = store_->AdvanceEpoch(next);
  if (!advanced.ok()) {
    return advanced;
  }
  live_epoch_ = next;
  boot_report_.epoch = live_epoch_;
  boot_report_.boot = live_boot_;
  boot_report_.notes.push_back(ReasonCode::RestartEpochAdvanced);
  boot_report_.notes.push_back(ReasonCode::RestartFreshnessNotRestored);
  boot_report_.notes.push_back(ReasonCode::RestartLeasesNotRestored);

  const Status fenced = CommitRestartFencing();
  if (!fenced.ok()) {
    return fenced;
  }
  if (boot_report_.attempts_fenced != 0) {
    boot_report_.notes.push_back(ReasonCode::RestartAttemptsFenced);
  }

  for (const auto& entry : store_->state().observations) {
    boot_report_.observations_retained += entry.second.size();
  }
  boot_report_.outcomes_retained = store_->state().outcomes.size();
  boot_report_.fences_active = store_->state().fences.size();
  compaction_watermark_ = store_->state().sequence;
  return Status::Ok();
}

Status ReconciliationEngine::CommitRestartFencing() {
  const UnixMillis now = WallClockMillis();
  std::vector<AttemptRecord> candidates;
  for (const auto& entry : store_->state().attempts) {
    if (!IsTerminalAttemptState(entry.second.state)) {
      candidates.push_back(entry.second);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const AttemptRecord& lhs, const AttemptRecord& rhs) { return lhs.id < rhs.id; });

  for (AttemptRecord& attempt : candidates) {
    // An attempt that never left the process cannot have produced an effect.
    // Anything at or beyond Dispatched may have reached the fabric, so the
    // runtime refuses to assume it did not.
    const bool may_have_effected = attempt.state != AttemptState::Issued;
    const AttemptState target =
        may_have_effected ? AttemptState::Interrupted : AttemptState::Abandoned;
    AttemptTransition transition;
    transition.from = attempt.state;
    transition.to = target;
    transition.epoch = live_epoch_;
    transition.boot = live_boot_;
    transition.at_unix_ms = now;
    transition.reason = may_have_effected ? ReasonCode::AttemptInterruptedByRestart
                                          : ReasonCode::AttemptAbandoned;
    attempt.state = target;
    if (attempt.transitions.size() < options_.limits.max_attempt_transitions) {
      attempt.transitions.push_back(transition);
    }
    const Status recorded = store_->TransitionAttempt(attempt);
    if (!recorded.ok()) {
      return recorded;
    }
    const ReconciliationOutcome outcome =
        MakeOutcome(attempt, target, transition.reason, may_have_effected, now);
    const Status committed = store_->CommitOutcome(outcome);
    if (!committed.ok()) {
      return committed;
    }
    if (may_have_effected) {
      ++boot_report_.attempts_interrupted;
    } else {
      ++boot_report_.attempts_abandoned;
    }
    ++boot_report_.attempts_fenced;
  }
  return Status::Ok();
}

ReasonCode ReconciliationEngine::FreshnessReason(FreshnessVerdict verdict) const noexcept {
  return ReasonOf(verdict);
}

bool ReconciliationEngine::FenceHoldsFor(const ScopeId& scope, const SubjectId& subject,
                                         CoordinatorEpoch epoch) const {
  for (const auto& entry : store_->state().fences) {
    const FenceEntry& fence = entry.second;
    if (!(fence.scope == scope)) {
      continue;
    }
    if (!fence.subject.empty() && !(fence.subject == subject)) {
      continue;
    }
    if (epoch < fence.fenced_below) {
      return true;
    }
  }
  return false;
}

AuthorityVector ReconciliationEngine::BuildAuthority(const ScopeEvidenceRef& evidence) const {
  AuthorityVector vector;
  const ReconciliationPolicy& policy = store_->state().policy;
  vector.coordinator_epoch = live_epoch_;
  vector.boot = live_boot_;
  vector.policy_id = policy.id;
  vector.policy_version = policy.version;
  vector.policy_digest = ComputePolicyDigest(policy);
  vector.definition = evidence.definition;
  vector.intent = evidence.intent;
  vector.intent_generation = evidence.intent_generation;
  vector.intent_digest = evidence.intent_digest;
  vector.observation = evidence.observation;
  vector.observation_generation = evidence.observation_generation;
  vector.observation_reporter = evidence.reporter;
  vector.observation_digest = evidence.observation_digest;
  vector.observation_epoch = evidence.received_epoch;
  vector.observation_received_unix_ms = evidence.received_unix_ms;
  return vector;
}

Status ReconciliationEngine::PutPolicy(const ReconciliationPolicy& policy,
                                       bool allow_same_version) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  ReconciliationPolicy canonical = policy;
  const Status status = CanonicalisePolicy(canonical, options_.limits);
  if (!status.ok()) {
    return status;
  }
  if (store_->state().has_policy) {
    const ReconciliationPolicy& current = store_->state().policy;
    if (canonical.version < current.version) {
      return Status(StatusCode::ConflictState, ReasonCode::IntentGenerationRegressed,
                    "policy version must not regress");
    }
    if (canonical.version == current.version) {
      if (allow_same_version && ComputePolicyDigest(canonical) == ComputePolicyDigest(current)) {
        return Status::Ok(ReasonCode::DuplicateIntentIdentical);
      }
      return Status(StatusCode::ConflictState, ReasonCode::ConflictingRepublishers,
                    "policy version already exists with different content");
    }
  }
  return store_->PutPolicy(canonical);
}

Result<ReconciliationPolicy> ReconciliationEngine::ActivePolicy() const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!store_->state().has_policy) {
    return Status(StatusCode::NotFound, ReasonCode::NoIntentForScope,
                  "store holds no policy yet");
  }
  return store_->state().policy;
}

Status ReconciliationEngine::CommitIntent(const IntentDocument& document,
                                          IntentCommitResult& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  IntentDocument candidate = document;
  const Status valid = ValidateIntent(candidate, options_.limits);
  if (!valid.ok()) {
    return valid;
  }
  candidate.digest = ComputeIntentDigest(candidate);
  out = IntentCommitResult{};
  out.generation = candidate.generation;
  out.digest = candidate.digest;

  const auto existing = store_->state().intents.find(candidate.scope);
  if (existing != store_->state().intents.end()) {
    if (candidate.generation < existing->second.generation) {
      return Status(StatusCode::Rejected, ReasonCode::IntentGenerationRegressed,
                    "intent generation is older than the committed intent");
    }
    if (candidate.generation == existing->second.generation) {
      if (candidate.digest == existing->second.digest) {
        out.duplicate = true;
        ++stats_.intent_duplicates;
        return Status::Ok(ReasonCode::DuplicateIntentIdentical);
      }
      ++stats_.intent_conflicts;
      return Status(StatusCode::ConflictState, ReasonCode::ConflictingRepublishers,
                    "intent generation already committed with different content");
    }
  }
  const Status committed = store_->CommitIntent(candidate);
  if (!committed.ok()) {
    return committed;
  }
  ++stats_.intents_committed;
  MaybeCompactLocked();
  return Status::Ok();
}

Status ReconciliationEngine::RecordObservation(const ObservationSubmission& submission,
                                               ObservationCommitResult& out) {
  return RecordObservationAt(submission, 0, out);
}

Status ReconciliationEngine::RecordObservationAt(const ObservationSubmission& submission,
                                                 UnixMillis stamp_unix_ms,
                                                 ObservationCommitResult& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  const Status valid = ValidateSubmission(submission, options_.limits);
  if (!valid.ok()) {
    return valid;
  }

  ObservationDocument document;
  document.observation = submission.observation;
  document.scope = submission.scope;
  document.reporter = submission.reporter;
  document.generation = submission.generation;
  document.evidence = submission.evidence;
  document.complete = submission.complete;
  document.authoritative_absence = submission.authoritative_absence;
  document.received_epoch = live_epoch_;
  document.received_unix_ms = (stamp_unix_ms != 0) ? stamp_unix_ms : WallClockMillis();
  document.subjects = submission.subjects;
  document.digest = ComputeObservationDigest(document);

  out = ObservationCommitResult{};
  out.stamped = document;

  const auto stream = store_->state().observations.find(submission.scope);
  if (stream != store_->state().observations.end()) {
    ObservationGeneration highest;
    bool seen = false;
    for (const ObservationDocument& retained : stream->second) {
      if (!(retained.reporter == submission.reporter)) {
        continue;
      }
      if (!seen || highest < retained.generation) {
        highest = retained.generation;
        seen = true;
      }
      if (retained.observation == submission.observation &&
          retained.generation == submission.generation) {
        if (retained.digest == document.digest) {
          out.duplicate = true;
          out.stamped = retained;
          ++stats_.observation_duplicates;
          return Status::Ok(ReasonCode::DuplicateObservationIdentical);
        }
        return Status(StatusCode::ConflictState, ReasonCode::ConflictingRepublishers,
                      "observation identity and generation already recorded with other content");
      }
    }
    if (seen && submission.generation < highest) {
      return Status(StatusCode::Rejected, ReasonCode::ObservationGenerationRegressed,
                    "observation generation is older than the recorded stream");
    }
  }

  const Status committed = store_->CommitObservation(document);
  if (!committed.ok()) {
    return committed;
  }
  ++stats_.observations_recorded;
  MaybeCompactLocked();
  return Status::Ok();
}

Result<ClassificationReport> ReconciliationEngine::Classify(const ScopeId& scope,
                                                             UnixMillis now_unix_ms) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!store_->state().has_policy) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::NoIntentForScope,
                  "store holds no policy");
  }
  ClassificationReport report;
  const Status status = ClassifyLocked(scope, now_unix_ms, report);
  if (!status.ok()) {
    return status;
  }
  return report;
}

Status ReconciliationEngine::ClassifyLocked(const ScopeId& scope, UnixMillis now_unix_ms,
                                            ClassificationReport& out) {
  std::vector<const IntentDocument*> intents;
  const auto intent = store_->state().intents.find(scope);
  if (intent != store_->state().intents.end()) {
    intents.push_back(&intent->second);
  }
  std::vector<const ObservationDocument*> observations;
  const auto stream = store_->state().observations.find(scope);
  if (stream != store_->state().observations.end()) {
    for (const ObservationDocument& document : stream->second) {
      observations.push_back(&document);
    }
  }

  ClassificationInput input;
  input.scope = scope;
  input.intents = &intents;
  input.observations = &observations;
  input.has_committed_intent = intent != store_->state().intents.end();
  if (input.has_committed_intent) {
    input.committed_intent_generation = intent->second.generation;
  }
  input.policy = &store_->state().policy;
  input.limits = &options_.limits;
  input.live_epoch = live_epoch_;
  input.now_unix_ms = (now_unix_ms != 0) ? now_unix_ms : WallClockMillis();

  ClassificationReport report;
  const Status status = ClassifyScope(input, report);
  if (!status.ok()) {
    return status;
  }
  stats_.subject_visits += report.subject_visits;
  stats_.attribute_comparisons += report.attribute_comparisons;
  out = std::move(report);
  return Status::Ok();
}

Result<ReconciliationPlan> ReconciliationEngine::Plan(const PlanRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  ReconciliationPlan plan;
  const Status status = BuildPlanLocked(request, plan);
  if (!status.ok()) {
    return status;
  }
  return plan;
}

Status ReconciliationEngine::BuildPlanLocked(const PlanRequest& request,
                                             ReconciliationPlan& out) {
  if (request.scope.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput, "plan requires a scope");
  }
  if (!store_->state().has_policy) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::NoIntentForScope,
                  "store holds no policy");
  }
  const ReconciliationPolicy& policy =
      (request.policy != nullptr) ? *request.policy : store_->state().policy;
  const UnixMillis now =
      (request.now_unix_ms != 0) ? request.now_unix_ms : WallClockMillis();

  std::vector<const IntentDocument*> intents;
  const auto intent = store_->state().intents.find(request.scope);
  if (intent != store_->state().intents.end()) {
    intents.push_back(&intent->second);
  }
  std::vector<const ObservationDocument*> observations;
  const auto stream = store_->state().observations.find(request.scope);
  if (stream != store_->state().observations.end()) {
    for (const ObservationDocument& document : stream->second) {
      observations.push_back(&document);
    }
  }

  ClassificationInput input;
  input.scope = request.scope;
  input.intents = &intents;
  input.observations = &observations;
  input.has_committed_intent = intent != store_->state().intents.end();
  if (input.has_committed_intent) {
    input.committed_intent_generation = intent->second.generation;
  }
  input.policy = &policy;
  input.limits = &options_.limits;
  input.live_epoch = live_epoch_;
  input.now_unix_ms = now;

  ClassificationReport report;
  const Status classified = ClassifyScope(input, report);
  if (!classified.ok()) {
    return classified;
  }
  stats_.subject_visits += report.subject_visits;
  stats_.attribute_comparisons += report.attribute_comparisons;

  const ScopeEvidenceRef& evidence = report.evidence;

  ReconciliationPlan plan;
  plan.scope = request.scope;
  plan.definition = evidence.definition;
  plan.epoch = live_epoch_;
  plan.boot = live_boot_;
  plan.policy_id = policy.id;
  plan.policy_version = policy.version;
  plan.policy_digest = ComputePolicyDigest(policy);
  plan.intent = evidence.intent;
  plan.intent_generation = evidence.intent_generation;
  plan.intent_digest = evidence.intent_digest;
  plan.observation = evidence.observation;
  plan.observation_generation = evidence.observation_generation;
  plan.reporter = evidence.reporter;
  plan.observation_digest = evidence.observation_digest;
  plan.observation_epoch = evidence.received_epoch;
  plan.observation_received_unix_ms = evidence.received_unix_ms;
  plan.freshness = evidence.freshness;
  plan.evidence = evidence.evidence;
  plan.scope_explanation = Explanation(options_.limits.max_explanation_steps);

  const AuthorityVector authority = BuildAuthority(evidence);
  const LiveAuthority live{live_epoch_, live_boot_, policy.id, policy.version,
                           plan.policy_digest};
  const bool freshness_ok = evidence.freshness == FreshnessVerdict::Fresh;
  const ReasonCode freshness_reason = ReasonOf(evidence.freshness);

  const IntentDocument* current_intent =
      intents.empty() ? nullptr : intents.front();
  const ObservationDocument* current_observation = nullptr;
  if (!observations.empty()) {
    const ObservationSelection selection = SelectCurrentObservation(observations);
    current_observation = selection.selected;
  }

  bool truncated = false;
  for (const DriftRecord& record : report.records) {
    if (plan.decisions.size() >= options_.limits.max_plan_actions) {
      truncated = true;
      break;
    }
    Decision decision;
    decision.scope = record.scope;
    decision.subject = record.subject;
    decision.drift = record.drift;
    decision.reason = record.reason;
    decision.evidence = record.evidence;
    decision.authority = authority;
    decision.explanation = Explanation(options_.limits.max_explanation_steps);
    decision.explanation.Add(record.reason);

    SubjectState desired;
    SubjectState expected_observed;
    if (current_intent != nullptr) {
      const auto declared = current_intent->subjects.find(record.subject);
      if (declared != current_intent->subjects.end()) {
        desired = declared->second.desired;
      }
    }
    if (current_observation != nullptr) {
      const auto seen = current_observation->subjects.find(record.subject);
      if (seen != current_observation->subjects.end()) {
        expected_observed = seen->second.observed;
      }
    }

    const bool fence_holds = FenceHoldsFor(record.scope, record.subject, live_epoch_);
    const AuthorityAssessment assessment =
        AssessAuthority(authority, live, freshness_reason, freshness_ok, fence_holds);
    decision.explanation.Add(assessment.proven ? ReasonCode::AuthorityProven
                                               : assessment.reason,
                             assessment.proven ? std::string()
                                               : assessment.RenderMask());

    switch (record.drift) {
      case DriftClass::AlreadyConverged:
        decision.action = ActionKind::None;
        decision.disposition = DecisionDisposition::AlreadySatisfied;
        break;
      case DriftClass::Missing:
        if (!policy.permit_apply_missing) {
          decision.action = ActionKind::None;
          decision.disposition = DecisionDisposition::Blocked;
          decision.reason = ReasonCode::PolicyForbidsApplyMissing;
          decision.explanation.Add(ReasonCode::PolicyForbidsApplyMissing);
        } else {
          decision.action = ActionKind::ApplyDesired;
          decision.disposition = DecisionDisposition::Actionable;
          decision.explanation.Add(ReasonCode::ActionEligible);
        }
        break;
      case DriftClass::Unexpected:
        if (!policy.permit_withdraw_unexpected) {
          decision.action = ActionKind::None;
          decision.disposition = DecisionDisposition::Blocked;
          decision.reason = ReasonCode::PolicyForbidsWithdrawUnexpected;
          decision.explanation.Add(ReasonCode::PolicyForbidsWithdrawUnexpected);
        } else {
          decision.action = ActionKind::WithdrawSubject;
          decision.disposition = DecisionDisposition::Actionable;
          decision.explanation.Add(ReasonCode::ActionEligible);
        }
        break;
      case DriftClass::Mismatched: {
        bool observe_only = false;
        bool actionable = false;
        for (const AttributeDelta& delta : record.deltas) {
          if (delta.kind == AttributeDeltaKind::ObserveOnly) {
            observe_only = true;
            decision.explanation.Add(ReasonCode::PolicyForbidsApplyMismatch,
                                     "observe-only attribute " + delta.key.str());
          } else if (delta.kind != AttributeDeltaKind::Unmanaged) {
            actionable = true;
            decision.explanation.Add(ReasonCode::ClassifiedAttributeMismatch,
                                     delta.key.str() + " intended=" + delta.intended.Render() +
                                         " observed=" + delta.observed.Render());
          }
        }
        if (!policy.permit_apply_mismatched) {
          decision.action = ActionKind::None;
          decision.disposition = DecisionDisposition::Blocked;
          decision.reason = ReasonCode::PolicyForbidsApplyMismatch;
          decision.explanation.Add(ReasonCode::PolicyForbidsApplyMismatch);
        } else if (actionable && !observe_only) {
          decision.action = ActionKind::ApplyDesired;
          decision.disposition = DecisionDisposition::Actionable;
          decision.explanation.Add(ReasonCode::ActionEligible);
        } else if (observe_only) {
          decision.action = ActionKind::None;
          decision.disposition = DecisionDisposition::Blocked;
          decision.reason = ReasonCode::PolicyForbidsApplyMismatch;
        } else {
          decision.action = ActionKind::None;
          decision.disposition = DecisionDisposition::AlreadySatisfied;
          decision.reason = ReasonCode::ClassifiedAlreadyConverged;
        }
        break;
      }
      case DriftClass::StaleObservation:
      case DriftClass::StaleIntent:
      case DriftClass::Unknown:
      case DriftClass::Invalid:
        decision.action = ActionKind::None;
        decision.disposition = DecisionDisposition::Indeterminate;
        decision.explanation.Add(ReasonCode::DriftIsIndeterminate);
        break;
      case DriftClass::Conflict:
        decision.action = ActionKind::None;
        decision.disposition = DecisionDisposition::Conflicted;
        decision.explanation.Add(ReasonCode::DriftIsConflicted);
        break;
      case DriftClass::Unsupported:
        decision.action = ActionKind::None;
        decision.disposition = DecisionDisposition::Unsupported;
        decision.explanation.Add(ReasonCode::SubjectKindUnsupported);
        break;
    }

    if (decision.disposition == DecisionDisposition::Actionable && !assessment.proven) {
      decision.disposition = fence_holds ? DecisionDisposition::Fenced
                                         : DecisionDisposition::Blocked;
      decision.action = ActionKind::None;
      decision.reason = assessment.reason;
      ++stats_.fenced_decisions;
    }

    decision.idempotency_key =
        ComputeIdempotencyKey(record.scope, record.subject, decision.action, authority, desired,
                              expected_observed);
    plan.decisions.push_back(std::move(decision));
  }

  // The verdict is a property of the whole scope, not only of the decisions.
  const bool any_indeterminate = [&plan]() {
    for (const Decision& decision : plan.decisions) {
      switch (decision.disposition) {
        case DecisionDisposition::Indeterminate:
        case DecisionDisposition::Conflicted:
        case DecisionDisposition::Unsupported:
        case DecisionDisposition::Invalid:
          return true;
        default:
          break;
      }
    }
    return false;
  }();
  const bool any_actionable = [&plan]() {
    for (const Decision& decision : plan.decisions) {
      if (decision.disposition == DecisionDisposition::Actionable) {
        return true;
      }
    }
    return false;
  }();
  const bool any_blocked = [&plan]() {
    for (const Decision& decision : plan.decisions) {
      if (decision.disposition == DecisionDisposition::Blocked ||
          decision.disposition == DecisionDisposition::Fenced) {
        return true;
      }
    }
    return false;
  }();

  if (truncated) {
    plan.verdict = ConvergenceVerdict::SearchLimitReached;
    plan.scope_reason = ReasonCode::PlanTruncated;
    plan.scope_explanation.Add(ReasonCode::PlanActionLimitReached,
                               "decisions bounded at " +
                                   std::to_string(options_.limits.max_plan_actions));
  } else if (!evidence.has_intent) {
    plan.verdict = ConvergenceVerdict::Indeterminate;
    plan.scope_reason = ReasonCode::NoIntentForScope;
    plan.scope_explanation.Add(ReasonCode::NoIntentForScope);
  } else if (!evidence.has_observation) {
    plan.verdict = ConvergenceVerdict::Indeterminate;
    plan.scope_reason = ReasonCode::NoObservationForScope;
    plan.scope_explanation.Add(ReasonCode::NoObservationForScope);
  } else if (!freshness_ok) {
    plan.verdict = ConvergenceVerdict::Indeterminate;
    plan.scope_reason = freshness_reason;
    plan.scope_explanation.Add(freshness_reason, ToText(evidence.freshness));
  } else if (any_indeterminate) {
    plan.verdict = ConvergenceVerdict::Indeterminate;
    plan.scope_reason = ReasonCode::DriftIsIndeterminate;
    plan.scope_explanation.Add(ReasonCode::DriftIsIndeterminate,
                               std::to_string(plan.CountDisposition(
                                   DecisionDisposition::Indeterminate)) +
                                   " indeterminate subjects");
  } else if (any_actionable) {
    plan.verdict = ConvergenceVerdict::ConvergenceRequired;
    plan.scope_reason = ReasonCode::ActionEligible;
    plan.scope_explanation.Add(ReasonCode::ActionEligible,
                               std::to_string(plan.MutationCount()) + " actions required");
  } else if (any_blocked) {
    plan.verdict = ConvergenceVerdict::ProvenBlocked;
    plan.scope_reason = ReasonCode::DriftIsConflicted;
    plan.scope_explanation.Add(ReasonCode::DriftIsConflicted,
                               "no legal plan reaches the intent under the active policy");
  } else {
    plan.verdict = ConvergenceVerdict::ConvergedProven;
    plan.scope_reason = ReasonCode::ClassifiedAlreadyConverged;
    plan.scope_explanation.Add(ReasonCode::ClassifiedAlreadyConverged,
                               std::to_string(plan.decisions.size()) + " subjects proven equal");
  }

  plan.truncated = truncated;
  plan.Seal();
  ++stats_.plans_built;
  out = std::move(plan);
  return Status::Ok();
}

Result<DispatchResult> ReconciliationEngine::Dispatch(const ReconciliationPlan& plan) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  if (!(plan.epoch == live_epoch_) || !(plan.boot == live_boot_)) {
    return Status(StatusCode::StaleAuthority, ReasonCode::AuthorityEpochMismatch,
                  "plan was built by a different incarnation or epoch");
  }
  if (!store_->state().has_policy) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::NoIntentForScope,
                  "store holds no policy");
  }
  const ReconciliationPolicy& policy = store_->state().policy;
  if (!(plan.policy_digest == ComputePolicyDigest(policy))) {
    return Status(StatusCode::StaleAuthority, ReasonCode::AuthorityPolicyMismatch,
                  "active policy changed since the plan was built");
  }
  const auto intent = store_->state().intents.find(plan.scope);
  if (intent == store_->state().intents.end() ||
      !(intent->second.generation == plan.intent_generation) ||
      !(intent->second.digest == plan.intent_digest)) {
    return Status(StatusCode::StaleAuthority, ReasonCode::AuthorityIntentGenerationMismatch,
                  "committed intent changed since the plan was built");
  }
  // The observation the plan was built against must still be the current
  // evidence. A newer generation makes every action in the plan revocable, so
  // the whole plan is refused rather than partially honoured.
  {
    std::vector<const ObservationDocument*> candidates;
    const auto stream = store_->state().observations.find(plan.scope);
    if (stream != store_->state().observations.end()) {
      for (const ObservationDocument& document : stream->second) {
        candidates.push_back(&document);
      }
    }
    const ObservationSelection selection = SelectCurrentObservation(candidates);
    if (selection.selected == nullptr ||
        !(selection.selected->observation == plan.observation) ||
        !(selection.selected->generation == plan.observation_generation) ||
        !(selection.selected->digest == plan.observation_digest)) {
      return Status(StatusCode::StaleAuthority,
                    ReasonCode::AuthorityObservationGenerationMismatch,
                    "the observation the plan was built against is no longer the current evidence");
    }
  }

  DispatchResult result;
  const UnixMillis now = WallClockMillis();
  for (const Decision& decision : plan.decisions) {
    if (decision.disposition != DecisionDisposition::Actionable) {
      continue;
    }
    if (FenceHoldsFor(decision.scope, decision.subject, live_epoch_)) {
      ++result.fenced;
      continue;
    }
    bool reused = false;
    for (const auto& entry : store_->state().attempts) {
      const AttemptRecord& existing = entry.second;
      if (existing.idempotency_key == decision.idempotency_key &&
          existing.epoch == live_epoch_ && !IsTerminalAttemptState(existing.state)) {
        ActionIntent intent_out;
        intent_out.scope = existing.scope;
        intent_out.subject = existing.subject;
        intent_out.action = existing.action;
        intent_out.authority = existing.authority;
        intent_out.idempotency_key = existing.idempotency_key;
        intent_out.attempt = existing.id;
        intent_out.epoch = existing.epoch;
        intent_out.boot = existing.boot;
        intent_out.issued_unix_ms = existing.issued_unix_ms;
        const auto declared = intent->second.subjects.find(existing.subject);
        if (declared != intent->second.subjects.end()) {
          intent_out.desired = declared->second.desired;
        }
        result.intents.push_back(std::move(intent_out));
        ++result.already_issued;
        reused = true;
        break;
      }
    }
    if (reused) {
      continue;
    }

    const std::uint64_t next_attempt = store_->state().next_attempt.value();
    const AttemptId id(next_attempt == 0 ? 1 : next_attempt);
    AttemptRecord attempt;
    attempt.id = id;
    attempt.idempotency_key = decision.idempotency_key;
    attempt.scope = decision.scope;
    attempt.subject = decision.subject;
    attempt.action = decision.action;
    attempt.epoch = live_epoch_;
    attempt.boot = live_boot_;
    attempt.authority = decision.authority;
    attempt.state = AttemptState::Dispatched;
    attempt.issued_unix_ms = now;
    AttemptTransition transition;
    transition.from = AttemptState::Issued;
    transition.to = AttemptState::Dispatched;
    transition.epoch = live_epoch_;
    transition.boot = live_boot_;
    transition.at_unix_ms = now;
    transition.reason = ReasonCode::AttemptIssued;
    attempt.transitions.push_back(transition);

    const Status issued = store_->IssueAttempt(attempt);
    if (!issued.ok()) {
      return issued;
    }
    ActionIntent action;
    action.scope = decision.scope;
    action.subject = decision.subject;
    action.action = decision.action;
    action.authority = decision.authority;
    action.idempotency_key = decision.idempotency_key;
    action.attempt = id;
    action.epoch = live_epoch_;
    action.boot = live_boot_;
    action.issued_unix_ms = now;
    const auto declared = intent->second.subjects.find(decision.subject);
    if (declared != intent->second.subjects.end()) {
      action.desired = declared->second.desired;
    }
    result.intents.push_back(std::move(action));
    ++stats_.actions_dispatched;
  }
  MaybeCompactLocked();
  return result;
}

ReconciliationOutcome ReconciliationEngine::MakeOutcome(const AttemptRecord& attempt,
                                                        AttemptState state, ReasonCode reason,
                                                        bool ambiguous,
                                                        UnixMillis now) const {
  ReconciliationOutcome outcome;
  outcome.idempotency_key = attempt.idempotency_key;
  outcome.scope = attempt.scope;
  outcome.subject = attempt.subject;
  outcome.action = attempt.action;
  outcome.attempt = attempt.id;
  outcome.epoch = attempt.epoch;
  outcome.boot = attempt.boot;
  outcome.terminal_state = state;
  outcome.reason = reason;
  outcome.committed_unix_ms = now;
  outcome.verified = (state == AttemptState::Verified);
  outcome.ambiguous = ambiguous;
  return outcome;
}

Status ReconciliationEngine::Complete(const CompletionRequest& request,
                                      ReconciliationOutcome& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  const auto found = store_->state().attempts.find(request.attempt.value());
  if (found == store_->state().attempts.end()) {
    return Status(StatusCode::NotFound, ReasonCode::AttemptUnknown,
                  "no such attempt in the retained lineage");
  }
  AttemptRecord attempt = found->second;
  if (!(attempt.idempotency_key == request.idempotency_key)) {
    return Status(StatusCode::Rejected, ReasonCode::AttemptUnknown,
                  "completion carries an idempotency key that does not match the attempt");
  }
  // The completer must act under the current incarnation. A completion that
  // claims a previous epoch or a previous boot identity is fenced before the
  // attempt itself is even inspected.
  if (!(request.epoch == live_epoch_) || !(request.boot == live_boot_)) {
    ++stats_.completions_rejected_late;
    return Status(StatusCode::StaleAuthority, ReasonCode::AttemptFencedByEpochAdvance,
                  "completion was reported under an epoch or boot identity that is not current");
  }
  if (!(attempt.epoch == live_epoch_) || !(attempt.boot == live_boot_)) {
    ++stats_.completions_rejected_late;
    return Status(StatusCode::StaleAuthority, ReasonCode::AttemptFencedByEpochAdvance,
                  "attempt belongs to a previous incarnation and is fenced");
  }
  if (IsTerminalAttemptState(attempt.state)) {
    ++stats_.duplicate_completions;
    return Status(StatusCode::ConflictState, ReasonCode::AttemptTerminalAlready,
                  "attempt already reached a terminal state; the completion is ignored");
  }
  if (!TransitionLegal(attempt.state, request.target)) {
    return Status(StatusCode::Rejected, ReasonCode::AttemptTransitionIllegal,
                  std::string("illegal transition from ") + ToText(attempt.state) + " to " +
                      ToText(request.target));
  }
  const UnixMillis now = (request.at_unix_ms != 0) ? request.at_unix_ms : WallClockMillis();
  AttemptTransition transition;
  transition.from = attempt.state;
  transition.to = request.target;
  transition.epoch = live_epoch_;
  transition.boot = live_boot_;
  transition.at_unix_ms = now;
  transition.reason = (request.reason != ReasonCode::None) ? request.reason
                                                           : ReasonOf(request.target);
  attempt.state = request.target;
  if (attempt.transitions.size() >= options_.limits.max_attempt_transitions) {
    attempt.transitions.erase(attempt.transitions.begin());
  }
  attempt.transitions.push_back(transition);

  const Status recorded = store_->TransitionAttempt(attempt);
  if (!recorded.ok()) {
    return recorded;
  }
  ++stats_.completions_accepted;

  if (IsTerminalAttemptState(attempt.state)) {
    const bool ambiguous = IsInconclusiveTerminalState(attempt.state);
    const ReconciliationOutcome outcome =
        MakeOutcome(attempt, attempt.state, transition.reason, ambiguous, now);
    const Status committed = store_->CommitOutcome(outcome);
    if (!committed.ok()) {
      return committed;
    }
    out = outcome;
  } else {
    out = MakeOutcome(attempt, attempt.state, transition.reason, false, now);
  }
  MaybeCompactLocked();
  return Status::Ok();
}

Status ReconciliationEngine::Verify(const VerificationEvidence& evidence,
                                    ReconciliationOutcome& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  const ReconciliationPolicy& policy = store_->state().policy;

  bool found_attempt = false;
  AttemptRecord stored;
  for (const auto& entry : store_->state().attempts) {
    if (entry.second.idempotency_key == evidence.idempotency_key) {
      stored = entry.second;
      found_attempt = true;
      break;
    }
  }
  if (!found_attempt) {
    return Status(StatusCode::NotFound, ReasonCode::AttemptUnknown,
                  "no attempt matches the supplied idempotency key");
  }
  AttemptRecord* target = &stored;
  if (!(target->scope == evidence.scope) || !(target->subject == evidence.subject)) {
    return Status(StatusCode::Rejected, ReasonCode::AttemptUnknown,
                  "verification evidence names a different subject");
  }
  if (!(target->epoch == live_epoch_) || !(target->boot == live_boot_)) {
    return Status(StatusCode::StaleAuthority, ReasonCode::AttemptFencedByEpochAdvance,
                  "attempt belongs to a previous incarnation and cannot be verified");
  }
  if (target->state != AttemptState::Applied) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::VerificationNotNewer,
                  std::string("verification requires an applied attempt, found ") +
                      ToText(target->state));
  }
  if (policy.require_independent_verification && evidence.verifier == evidence.applier) {
    ++stats_.verifications_rejected;
    return Status(StatusCode::Rejected, ReasonCode::VerificationNotIndependent,
                  "verification must come from a reporter other than the applier");
  }
  if (!(evidence.received_epoch == live_epoch_)) {
    ++stats_.verifications_rejected;
    return Status(StatusCode::StaleAuthority, ReasonCode::ObservationForeignEpoch,
                  "verification evidence was recorded by a previous incarnation");
  }
  if (!(target->authority.observation_generation < evidence.generation)) {
    ++stats_.verifications_rejected;
    return Status(StatusCode::StaleAuthority, ReasonCode::VerificationNotNewer,
                  "verification evidence is not newer than the observation the action used");
  }

  // Verification is only accepted against evidence the runtime actually holds.
  const ObservationDocument* recorded = nullptr;
  const auto stream = store_->state().observations.find(evidence.scope);
  if (stream != store_->state().observations.end()) {
    for (const ObservationDocument& document : stream->second) {
      if (document.observation == evidence.observation &&
          document.generation == evidence.generation &&
          document.reporter == evidence.verifier) {
        recorded = &document;
        break;
      }
    }
  }
  if (recorded == nullptr) {
    ++stats_.verifications_rejected;
    return Status(StatusCode::NotFound, ReasonCode::NoObservationForScope,
                  "verification cites observation evidence the runtime does not hold");
  }
  const FreshnessVerdict freshness =
      EvaluateFreshness(*recorded, policy.freshness, live_epoch_, WallClockMillis());
  if (freshness != FreshnessVerdict::Fresh) {
    ++stats_.verifications_rejected;
    return Status(StatusCode::StaleAuthority, ReasonOf(freshness),
                  "verification evidence is not fresh");
  }

  const UnixMillis now = (evidence.received_unix_ms != 0) ? evidence.received_unix_ms
                                                          : WallClockMillis();
  const AttemptState next = evidence.confirms_effect ? AttemptState::Verified
                                                     : AttemptState::Failed;
  AttemptTransition transition;
  transition.from = target->state;
  transition.to = next;
  transition.epoch = live_epoch_;
  transition.boot = live_boot_;
  transition.at_unix_ms = now;
  transition.reason = evidence.confirms_effect ? ReasonCode::AttemptVerified
                                               : ReasonCode::AttemptFailed;
  transition.verification_reason = evidence.confirms_effect ? ReasonCode::AttemptVerified
                                                            : ReasonCode::VerificationNotNewer;
  target->state = next;
  if (target->transitions.size() >= options_.limits.max_attempt_transitions) {
    target->transitions.erase(target->transitions.begin());
  }
  target->transitions.push_back(transition);

  const Status recorded_status = store_->TransitionAttempt(*target);
  if (!recorded_status.ok()) {
    return recorded_status;
  }
  ReconciliationOutcome outcome =
      MakeOutcome(*target, next, transition.reason, !evidence.confirms_effect, now);
  outcome.verified_generation = evidence.generation;
  const Status committed = store_->CommitOutcome(outcome);
  if (!committed.ok()) {
    return committed;
  }
  ++stats_.verifications_accepted;
  out = outcome;
  MaybeCompactLocked();
  return Status::Ok();
}

Status ReconciliationEngine::PutFence(const FenceEntry& fence) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  if (fence.id.empty() || fence.scope.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "fence requires a non-empty identity and scope");
  }
  if (fence.fenced_below.value() == 0) {
    return Status(StatusCode::Rejected, ReasonCode::FenceEpochNotNewer,
                  "a fence below epoch zero would revoke nothing");
  }
  const UnixMillis now = (fence.created_unix_ms != 0) ? fence.created_unix_ms
                                                      : WallClockMillis();
  FenceEntry entry = fence;
  entry.created_unix_ms = now;
  const Status stored = store_->PutFence(entry);
  if (!stored.ok()) {
    return stored;
  }
  // A fence is only meaningful if it actually revokes in-flight authority.
  std::vector<AttemptRecord> victims;
  for (const auto& candidate : store_->state().attempts) {
    const AttemptRecord& attempt = candidate.second;
    if (IsTerminalAttemptState(attempt.state)) {
      continue;
    }
    if (!(attempt.scope == entry.scope)) {
      continue;
    }
    if (!entry.subject.empty() && !(entry.subject == attempt.subject)) {
      continue;
    }
    if (attempt.epoch < entry.fenced_below) {
      victims.push_back(attempt);
    }
  }
  std::sort(victims.begin(), victims.end(),
            [](const AttemptRecord& lhs, const AttemptRecord& rhs) { return lhs.id < rhs.id; });
  for (AttemptRecord& attempt : victims) {
    AttemptTransition transition;
    transition.from = attempt.state;
    transition.to = AttemptState::Fenced;
    transition.epoch = live_epoch_;
    transition.boot = live_boot_;
    transition.at_unix_ms = now;
    transition.reason = ReasonCode::AttemptFencedByEpochAdvance;
    attempt.state = AttemptState::Fenced;
    if (attempt.transitions.size() >= options_.limits.max_attempt_transitions) {
      attempt.transitions.erase(attempt.transitions.begin());
    }
    attempt.transitions.push_back(transition);
    const Status recorded = store_->TransitionAttempt(attempt);
    if (!recorded.ok()) {
      return recorded;
    }
    const ReconciliationOutcome outcome =
        MakeOutcome(attempt, AttemptState::Fenced, transition.reason, false, now);
    const Status committed = store_->CommitOutcome(outcome);
    if (!committed.ok()) {
      return committed;
    }
    ++boot_report_.attempts_fenced;
  }
  MaybeCompactLocked();
  return Status::Ok();
}

Status ReconciliationEngine::ClearFence(const FenceId& fence) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  if (store_->state().fences.find(fence) == store_->state().fences.end()) {
    return Status(StatusCode::NotFound, ReasonCode::None, "no such fence");
  }
  return store_->ClearFence(fence);
}

Result<std::vector<FenceEntry>> ReconciliationEngine::ActiveFences(const ScopeId& scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<FenceEntry> fences;
  for (const auto& entry : store_->state().fences) {
    if (scope.empty() || entry.second.scope == scope) {
      fences.push_back(entry.second);
    }
  }
  return fences;
}

Result<std::vector<ScopeId>> ReconciliationEngine::KnownScopes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ScopeId> scopes;
  for (const auto& entry : store_->state().intents) {
    scopes.push_back(entry.first);
  }
  for (const auto& entry : store_->state().observations) {
    if (std::find(scopes.begin(), scopes.end(), entry.first) == scopes.end()) {
      scopes.push_back(entry.first);
    }
  }
  std::sort(scopes.begin(), scopes.end());
  return scopes;
}

Result<std::vector<AttemptRecord>> ReconciliationEngine::Attempts(const ScopeId& scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AttemptRecord> attempts;
  for (const auto& entry : store_->state().attempts) {
    if (scope.empty() || entry.second.scope == scope) {
      attempts.push_back(entry.second);
    }
  }
  std::sort(attempts.begin(), attempts.end(),
            [](const AttemptRecord& lhs, const AttemptRecord& rhs) { return lhs.id < rhs.id; });
  return attempts;
}

Result<std::vector<ReconciliationOutcome>> ReconciliationEngine::Outcomes(
    const ScopeId& scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ReconciliationOutcome> outcomes;
  for (const ReconciliationOutcome& outcome : store_->state().outcomes) {
    if (scope.empty() || outcome.scope == scope) {
      outcomes.push_back(outcome);
    }
  }
  return outcomes;
}

Result<std::vector<ObservationDocument>> ReconciliationEngine::ObservationEvidence(
    const ScopeId& scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ObservationDocument> documents;
  const auto stream = store_->state().observations.find(scope);
  if (stream != store_->state().observations.end()) {
    for (const ObservationDocument& document : stream->second) {
      documents.push_back(document);
    }
  }
  return documents;
}

Result<IntentDocument> ReconciliationEngine::CurrentIntent(const ScopeId& scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = store_->state().intents.find(scope);
  if (found == store_->state().intents.end()) {
    return Status(StatusCode::NotFound, ReasonCode::NoIntentForScope,
                  "no committed intent for this scope");
  }
  return found->second;
}

Status ReconciliationEngine::Compact() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (store_->read_only()) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "engine is read-only");
  }
  const Status status = store_->Compact();
  if (status.ok()) {
    ++stats_.compactions;
    compaction_watermark_ = store_->state().sequence;
  }
  return status;
}

Status ReconciliationEngine::Flush() {
  std::lock_guard<std::mutex> guard(mutex_);
  return store_->SyncJournal();
}

void ReconciliationEngine::MaybeCompactLocked() {
  if (options_.auto_compact_after_records == 0) {
    return;
  }
  const std::uint64_t delta =
      store_->state().sequence.value() - compaction_watermark_.value();
  if (delta < options_.auto_compact_after_records) {
    return;
  }
  if (store_->Compact().ok()) {
    ++stats_.compactions;
    compaction_watermark_ = store_->state().sequence;
  }
}

CoordinatorEpoch ReconciliationEngine::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return live_epoch_;
}

BootId ReconciliationEngine::boot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return live_boot_;
}

StoreId ReconciliationEngine::store_id() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return store_->state().store;
}

EngineStats ReconciliationEngine::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_;
}

std::string RenderPlanText(const ReconciliationPlan& plan) {
  std::ostringstream out;
  out << "plan scope=" << plan.scope.str() << " id=" << plan.plan_id << "\n";
  out << "  epoch=" << plan.epoch.value() << " boot=" << plan.boot.ToHex().substr(0, 8)
      << " verdict=" << ToText(plan.verdict) << " evidence=" << ToText(plan.evidence) << "\n";
  out << "  policy=" << plan.policy_id.str() << "/" << plan.policy_version.value()
      << " intent=" << plan.intent.str() << "/g" << plan.intent_generation.value()
      << " observation=" << plan.observation.str() << "/g"
      << plan.observation_generation.value() << " freshness=" << ToText(plan.freshness) << "\n";
  out << "  decisions=" << plan.decisions.size() << " actions=" << plan.MutationCount()
      << " truncated=" << (plan.truncated ? "yes" : "no") << "\n";
  if (!plan.scope_explanation.steps().empty()) {
    out << "  scope explanation:\n";
    for (const ExplanationStep& step : plan.scope_explanation.steps()) {
      out << "    " << ToText(step.code);
      if (!step.detail.empty()) {
        out << ": " << step.detail;
      }
      out << "\n";
    }
  }
  for (const Decision& decision : plan.decisions) {
    out << "  - " << decision.subject.str() << " drift=" << ToText(decision.drift)
        << " disposition=" << ToText(decision.disposition) << " action=" << ToText(decision.action)
        << " key=" << ToHex(decision.idempotency_key).substr(0, 16) << "\n";
    for (const ExplanationStep& step : decision.explanation.steps()) {
      out << "      " << ToText(step.code);
      if (!step.detail.empty()) {
        out << ": " << step.detail;
      }
      out << "\n";
    }
  }
  return out.str();
}

std::string RenderClassificationText(const ClassificationReport& report) {
  std::ostringstream out;
  out << "classification scope=" << report.scope.str()
      << " subjects=" << report.records.size()
      << " visits=" << report.subject_visits
      << " attribute-comparisons=" << report.attribute_comparisons << "\n";
  out << "  intent=" << (report.evidence.has_intent ? report.evidence.intent.str() : "<none>")
      << " observation="
      << (report.evidence.has_observation ? report.evidence.observation.str() : "<none>")
      << " freshness=" << ToText(report.evidence.freshness) << "\n";
  for (const DriftRecord& record : report.records) {
    out << "  - " << record.subject.str() << " " << ToText(record.drift) << " ("
        << ToText(record.reason) << ")\n";
    for (const AttributeDelta& delta : record.deltas) {
      out << "      " << delta.key.str() << " " << ToText(delta.kind) << " intended="
          << delta.intended.Render() << " observed=" << delta.observed.Render() << "\n";
    }
  }
  return out.str();
}

std::string RenderBootReportText(const BootReport& report) {
  std::ostringstream out;
  out << "boot store=" << report.store.ToHex().substr(0, 8)
      << " boot=" << report.boot.ToHex().substr(0, 8)
      << " epoch=" << report.epoch.value() << " previous-epoch=" << report.previous_epoch.value()
      << "\n";
  out << "  created=" << (report.store_created ? "yes" : "no")
      << " snapshot=" << (report.snapshot_loaded ? "yes" : "no")
      << " torn-tail=" << (report.recovered_torn_tail ? "yes" : "no")
      << " recovered-bytes=" << report.recovered_bytes
      << " replayed=" << report.records_replayed << "\n";
  out << "  attempts restored=" << report.attempts_restored
      << " interrupted=" << report.attempts_interrupted
      << " abandoned=" << report.attempts_abandoned << " fenced=" << report.attempts_fenced
      << "\n";
  out << "  observations retained=" << report.observations_retained
      << " outcomes=" << report.outcomes_retained << " fences=" << report.fences_active << "\n";
  out << "  freshness-restored=" << (report.freshness_restored ? "yes" : "no")
      << " mutation-authority-restored=" << (report.mutation_authority_restored ? "yes" : "no")
      << "\n";
  for (const ReasonCode note : report.notes) {
    out << "  note " << ToText(note) << "\n";
  }
  return out.str();
}

std::string RenderAttemptText(const AttemptRecord& attempt) {
  std::ostringstream out;
  out << "attempt id=" << attempt.id.value() << " scope=" << attempt.scope.str()
      << " subject=" << attempt.subject.str() << " action=" << ToText(attempt.action)
      << " state=" << ToText(attempt.state) << " epoch=" << attempt.epoch.value()
      << " boot=" << attempt.boot.ToHex().substr(0, 8) << "\n";
  out << "  key=" << ToHex(attempt.idempotency_key) << "\n";
  for (const AttemptTransition& transition : attempt.transitions) {
    out << "  " << ToText(transition.from) << " -> " << ToText(transition.to)
        << " epoch=" << transition.epoch.value() << " reason=" << ToText(transition.reason)
        << "\n";
  }
  return out.str();
}

}  // namespace fabric_reconciliation
}  // namespace summon
