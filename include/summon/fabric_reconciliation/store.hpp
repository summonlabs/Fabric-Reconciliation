// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store: the legitimate durable subset of runtime state.
//
// Durable by design:
//   * the active policy and its version;
//   * committed intents and their generation lineage;
//   * recorded observation evidence (as evidence, never as freshness);
//   * attempt lineage and committed reconciliation outcomes;
//   * fences;
//   * the coordinator epoch floor and the record sequence.
//
// Not durable, and deliberately not reconstructed from disk:
//   * telemetry freshness;
//   * live leases, grants and in-flight mutation authority;
//   * backend effects implied by an attempt record.
//
// Every mutation appends and fsyncs a record before the in-memory state is
// changed, so a crash can only lose a mutation that was never acknowledged.

#ifndef SUMMON_FABRIC_RECONCILIATION_STORE_HPP
#define SUMMON_FABRIC_RECONCILIATION_STORE_HPP

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/document.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/journal.hpp"
#include "summon/fabric_reconciliation/lineage.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/policy.hpp"
#include "summon/fabric_reconciliation/snapshot.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Everything that legitimately survives a restart.
struct FR_API DurableState {
  StoreId store;
  CoordinatorEpoch last_epoch;
  RecordSequence sequence;
  /// Next attempt identity to issue. Attempt identity zero is never valid, so
  /// the first issued attempt is 1.
  AttemptId next_attempt{1};

  bool has_policy{false};
  ReconciliationPolicy policy;

  std::map<ScopeId, IntentDocument> intents;
  std::map<ScopeId, std::vector<IntentGeneration>> intent_generations;
  std::map<ScopeId, std::deque<ObservationDocument>> observations;
  std::map<std::uint64_t, AttemptRecord> attempts;
  std::deque<ReconciliationOutcome> outcomes;
  std::map<FenceId, FenceEntry> fences;

  std::uint64_t compactions{0};
};

/// What happened while opening the store.
struct FR_API StoreOpenReport {
  bool created{false};
  bool snapshot_loaded{false};
  bool recovered_torn_tail{false};
  std::uint64_t recovered_bytes{0};
  std::size_t records_replayed{0};
  std::size_t records_skipped_by_watermark{0};
  RecordSequence last_sequence;
  CoordinatorEpoch last_epoch;
  std::size_t attempts_restored{0};
  std::vector<ReasonCode> notes;
};

/// The durable half of the runtime. Thread-compatible but not thread-safe:
/// the engine owns it and serialises access.
class FR_API DurableStore {
 public:
  DurableStore() = default;
  DurableStore(DurableStore&& other) noexcept;
  DurableStore& operator=(DurableStore&& other) noexcept;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  ~DurableStore();

  /// Opens or creates a store rooted at @p directory. Creates the directory
  /// when missing and when @p create_if_missing is set.
  [[nodiscard]] static Status Open(const std::filesystem::path& directory,
                                   const RuntimeLimits& limits, bool create_if_missing,
                                   bool read_only, DurableStore& out, StoreOpenReport& report);

  [[nodiscard]] const DurableState& state() const noexcept { return state_; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  [[nodiscard]] const RuntimeLimits& limits() const noexcept { return limits_; }

  // ---- durable mutations; durable first, memory second ----
  [[nodiscard]] Status PutPolicy(const ReconciliationPolicy& policy);
  [[nodiscard]] Status CommitIntent(const IntentDocument& document);
  [[nodiscard]] Status CommitObservation(const ObservationDocument& document);
  [[nodiscard]] Status IssueAttempt(const AttemptRecord& attempt);
  [[nodiscard]] Status TransitionAttempt(const AttemptRecord& attempt);
  [[nodiscard]] Status CommitOutcome(const ReconciliationOutcome& outcome);
  [[nodiscard]] Status PutFence(const FenceEntry& fence);
  [[nodiscard]] Status ClearFence(const FenceId& fence);
  [[nodiscard]] Status AdvanceEpoch(CoordinatorEpoch epoch);

  /// Compacts: publishes a snapshot durably, then starts a fresh journal whose
  /// first record continues the sequence above the snapshot watermark.
  [[nodiscard]] Status Compact();

  /// Flushes any buffered durable bytes to the device without compacting.
  [[nodiscard]] Status SyncJournal();

  [[nodiscard]] RecordSequence last_sequence() const noexcept { return state_.sequence; }
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_.bytes_written(); }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }
  [[nodiscard]] const std::filesystem::path& journal_path() const noexcept { return journal_path_; }
  [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
    return snapshot_path_;
  }
  [[nodiscard]] const std::filesystem::path& staging_path() const noexcept { return staging_path_; }
  [[nodiscard]] const std::filesystem::path& lock_path() const noexcept { return lock_path_; }

  /// Releases the exclusive store lock and closes the journal.
  void Close() noexcept;

  /// Immediately truncates and reopens the journal file, simulating a crash
  /// that lost buffered bytes. Test support only; never called by the engine.
  [[nodiscard]] Status ReopenJournalForTesting();

 private:
  [[nodiscard]] Status ApplyRecord(const JournalRecord& record);
  [[nodiscard]] Status ApplyPayload(RecordType type, std::string_view payload);
  [[nodiscard]] Status AppendAndApply(RecordType type, std::string_view payload);
  [[nodiscard]] std::string EncodeSnapshotPayload() const;
  [[nodiscard]] Status ApplySnapshotPayload(std::string_view payload);
  void TrimObservations(const ScopeId& scope);
  void TrimAttempts();
  void TrimOutcomes();

  std::filesystem::path directory_;
  std::filesystem::path journal_path_;
  std::filesystem::path snapshot_path_;
  std::filesystem::path staging_path_;
  std::filesystem::path lock_path_;
  /// Operating-system handle for the exclusive store lock. Never persisted:
  /// liveness is a property of the live process, not of a file on disk.
  void* lock_handle_{nullptr};
  RuntimeLimits limits_{};
  DurableState state_;
  Journal journal_;
  bool read_only_{false};
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_STORE_HPP
