// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The reconciliation runtime.
//
// One engine owns one process incarnation: a coordinator epoch, a boot
// identity, the durable store, and the live dynamic tables that a restart is
// not allowed to restore. The engine is the only place where authority is
// proven and where mutation intent is minted.

#ifndef SUMMON_FABRIC_RECONCILIATION_ENGINE_HPP
#define SUMMON_FABRIC_RECONCILIATION_ENGINE_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/drift.hpp"
#include "summon/fabric_reconciliation/lineage.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/plan.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/policy.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/store.hpp"

namespace summon {
namespace fabric_reconciliation {

struct FR_API EngineOptions {
  std::filesystem::path store_dir;
  RuntimeLimits limits{};
  bool create_if_missing{true};
  bool read_only{false};
  /// Installed when the store holds no policy yet. Ignored otherwise, so a
  /// restart can never silently replace a durable policy.
  ReconciliationPolicy initial_policy{};
  bool install_initial_policy{true};
  /// Compact automatically once this many records have been appended since the
  /// last compaction. Zero disables automatic compaction.
  std::uint64_t auto_compact_after_records{8192};
};

/// What the runtime tells the world about its own restart behaviour.
struct FR_API BootReport {
  StoreId store;
  CoordinatorEpoch previous_epoch;
  CoordinatorEpoch epoch;
  BootId boot;
  bool store_created{false};
  bool snapshot_loaded{false};
  bool recovered_torn_tail{false};
  std::uint64_t recovered_bytes{0};
  std::size_t records_replayed{0};
  std::size_t attempts_restored{0};
  std::size_t attempts_interrupted{0};
  std::size_t attempts_abandoned{0};
  std::size_t attempts_fenced{0};
  std::size_t observations_retained{0};
  std::size_t outcomes_retained{0};
  std::size_t fences_active{0};
  /// Always false. Freshness is a property of the live process.
  bool freshness_restored{false};
  /// Always false. Mutation authority never survives a restart.
  bool mutation_authority_restored{false};
  std::vector<ReasonCode> notes;
};

/// Result of committing an intent.
struct FR_API IntentCommitResult {
  bool duplicate{false};
  IntentGeneration generation;
  Sha256Digest digest{};
};

/// Result of recording an observation.
struct FR_API ObservationCommitResult {
  bool duplicate{false};
  ObservationDocument stamped;
};

/// A completion reported for an attempt that the runtime previously issued.
struct FR_API CompletionRequest {
  AttemptId attempt;
  /// Authority the completer believes it is acting under. Both fields must
  /// match live authority or the completion is fenced.
  CoordinatorEpoch epoch;
  BootId boot;
  Sha256Digest idempotency_key{};
  AttemptState target{AttemptState::Invalid};
  UnixMillis at_unix_ms{0};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

/// A dispatched batch.
struct FR_API DispatchResult {
  std::vector<ActionIntent> intents;
  std::size_t fenced{0};
  std::size_t superseded{0};
  std::size_t already_issued{0};
};

struct FR_API EngineStats {
  std::uint64_t intents_committed{0};
  std::uint64_t intent_duplicates{0};
  std::uint64_t intent_conflicts{0};
  std::uint64_t observations_recorded{0};
  std::uint64_t observation_duplicates{0};
  std::uint64_t plans_built{0};
  std::uint64_t actions_dispatched{0};
  std::uint64_t completions_accepted{0};
  std::uint64_t completions_rejected_late{0};
  std::uint64_t duplicate_completions{0};
  std::uint64_t verifications_accepted{0};
  std::uint64_t verifications_rejected{0};
  std::uint64_t subject_visits{0};
  std::uint64_t attribute_comparisons{0};
  std::uint64_t fenced_decisions{0};
  std::uint64_t compactions{0};
};

/// The reconciliation engine.
///
/// Thread-safe. Exactly one internal mutex guards durable state and the live
/// tables. No callback, no logging hook and no transport call is ever made
/// while that mutex is held, no helper that re-enters the engine is called
/// with it held, and no thread is joined while it is held. The ownership audit
/// in docs/CONCURRENCY-AUDIT.md records the reasoning and the checks.
class FR_API ReconciliationEngine {
 public:
  ~ReconciliationEngine();
  ReconciliationEngine(const ReconciliationEngine&) = delete;
  ReconciliationEngine& operator=(const ReconciliationEngine&) = delete;

  [[nodiscard]] static Status Open(const EngineOptions& options,
                                   std::unique_ptr<ReconciliationEngine>& out);

  // ---- identity ----
  [[nodiscard]] const BootReport& boot_report() const noexcept { return boot_report_; }
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] BootId boot() const;
  [[nodiscard]] StoreId store_id() const;
  [[nodiscard]] EngineStats stats() const;

  // ---- durable definition and policy ----
  [[nodiscard]] Status PutPolicy(const ReconciliationPolicy& policy, bool allow_same_version = false);
  [[nodiscard]] Result<ReconciliationPolicy> ActivePolicy() const;

  [[nodiscard]] Status CommitIntent(const IntentDocument& document, IntentCommitResult& out);
  [[nodiscard]] Status RecordObservation(const ObservationSubmission& submission,
                                         ObservationCommitResult& out);

  /// Records an observation with an explicit receipt stamp. A stamp of zero
  /// means "use the host clock", which is the production path; a non-zero stamp
  /// exists so that freshness proofs are reproducible and so that the caller
  /// can replay recorded evidence. The receipt epoch is always the live epoch
  /// and can never be supplied by the caller.
  [[nodiscard]] Status RecordObservationAt(const ObservationSubmission& submission,
                                           UnixMillis stamp_unix_ms,
                                           ObservationCommitResult& out);

  // ---- classification and planning ----
  [[nodiscard]] Result<ClassificationReport> Classify(const ScopeId& scope, UnixMillis now_unix_ms);
  [[nodiscard]] Result<ReconciliationPlan> Plan(const PlanRequest& request);
  [[nodiscard]] Result<DispatchResult> Dispatch(const ReconciliationPlan& plan);

  // ---- attempt lifecycle ----
  [[nodiscard]] Status Complete(const CompletionRequest& request, ReconciliationOutcome& out);
  [[nodiscard]] Status Verify(const VerificationEvidence& evidence, ReconciliationOutcome& out);

  // ---- authority ----
  [[nodiscard]] Status PutFence(const FenceEntry& fence);
  [[nodiscard]] Status ClearFence(const FenceId& fence);
  [[nodiscard]] Result<std::vector<FenceEntry>> ActiveFences(const ScopeId& scope) const;

  // ---- introspection ----
  [[nodiscard]] Result<std::vector<ScopeId>> KnownScopes() const;
  [[nodiscard]] Result<std::vector<AttemptRecord>> Attempts(const ScopeId& scope) const;
  [[nodiscard]] Result<std::vector<ReconciliationOutcome>> Outcomes(const ScopeId& scope) const;
  [[nodiscard]] Result<std::vector<ObservationDocument>> ObservationEvidence(
      const ScopeId& scope) const;
  [[nodiscard]] Result<IntentDocument> CurrentIntent(const ScopeId& scope) const;

  // ---- durability control ----
  [[nodiscard]] Status Compact();
  [[nodiscard]] Status Flush();

 private:
  ReconciliationEngine() = default;

  struct LiveState;

  [[nodiscard]] Status Boot(const EngineOptions& options);
  [[nodiscard]] Status CommitRestartFencing();
  [[nodiscard]] ReasonCode FreshnessReason(FreshnessVerdict verdict) const noexcept;
  [[nodiscard]] bool FenceHoldsFor(const ScopeId& scope, const SubjectId& subject,
                                   CoordinatorEpoch epoch) const;
  [[nodiscard]] AuthorityVector BuildAuthority(const ScopeEvidenceRef& evidence) const;
  [[nodiscard]] Status BuildPlanLocked(const PlanRequest& request, ReconciliationPlan& out);
  [[nodiscard]] Status ClassifyLocked(const ScopeId& scope, UnixMillis now_unix_ms,
                                      ClassificationReport& out);
  [[nodiscard]] ReconciliationOutcome MakeOutcome(const AttemptRecord& attempt, AttemptState state,
                                                  ReasonCode reason, bool ambiguous,
                                                  UnixMillis now) const;
  /// Compacts when the append count since the last compaction exceeds the
  /// configured bound. Called with the engine mutex held.
  void MaybeCompactLocked();

  mutable std::mutex mutex_;
  std::unique_ptr<DurableStore> store_;
  EngineOptions options_{};
  BootReport boot_report_{};
  EngineStats stats_{};
  /// Live, non-durable: the epoch this process incarnation owns.
  CoordinatorEpoch live_epoch_;
  BootId live_boot_;
  /// Attempt identifiers issued by this incarnation only.
  AttemptId live_attempt_;
  /// Durable sequence at the last compaction, used to bound journal growth.
  RecordSequence compaction_watermark_;
};

/// Renders a plan as deterministic multi-line text.
[[nodiscard]] FR_API std::string RenderPlanText(const ReconciliationPlan& plan);

/// Renders a classification report as deterministic multi-line text.
[[nodiscard]] FR_API std::string RenderClassificationText(const ClassificationReport& report);

/// Renders a boot report as deterministic multi-line text.
[[nodiscard]] FR_API std::string RenderBootReportText(const BootReport& report);

/// Renders an attempt record as deterministic multi-line text.
[[nodiscard]] FR_API std::string RenderAttemptText(const AttemptRecord& attempt);

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_ENGINE_HPP
