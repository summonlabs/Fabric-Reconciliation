// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Attempt lineage, committed outcomes and fences.
//
// Acknowledgement is not application. Application is not verified effect. The
// runtime keeps those three states separate in the type system and in durable
// storage, and it never promotes one into another without evidence.

#ifndef SUMMON_FABRIC_RECONCILIATION_LINEAGE_HPP
#define SUMMON_FABRIC_RECONCILIATION_LINEAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/authority.hpp"
#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/plan.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Lifecycle of one action intent.
///
/// Issued -> Dispatched -> Acknowledged -> Applied -> Verified is the happy
/// path. Failed, Abandoned, Superseded, Fenced and Interrupted are terminal.
/// Interrupted is produced by restart: an attempt that was in flight when the
/// process died is never assumed complete.
enum class AttemptState : std::uint8_t {
  Invalid = 0,
  Issued = 1,
  Dispatched = 2,
  Acknowledged = 3,
  Applied = 4,
  Verified = 5,
  Failed = 6,
  Abandoned = 7,
  Superseded = 8,
  Fenced = 9,
  Interrupted = 10,
};

[[nodiscard]] FR_API const char* ToText(AttemptState value) noexcept;
[[nodiscard]] FR_API bool TryParseAttemptState(const char* text, AttemptState& out) noexcept;
[[nodiscard]] FR_API bool IsTerminalAttemptState(AttemptState value) noexcept;
/// Applies no effect and claims no success. Interrupted, Fenced, Superseded,
/// Abandoned and Failed are all inconclusive: the runtime does not know
/// whether bytes reached the fabric.
[[nodiscard]] FR_API bool IsInconclusiveTerminalState(AttemptState value) noexcept;
[[nodiscard]] FR_API ReasonCode ReasonOf(AttemptState value) noexcept;

/// One lifecycle transition. History is bounded per attempt.
struct FR_API AttemptTransition {
  AttemptState from{AttemptState::Invalid};
  AttemptState to{AttemptState::Invalid};
  CoordinatorEpoch epoch;
  BootId boot;
  UnixMillis at_unix_ms{0};
  ReasonCode reason{ReasonCode::None};
  ReasonCode verification_reason{ReasonCode::None};

  friend bool operator==(const AttemptTransition&, const AttemptTransition&) = default;
};

/// A durable action intent and its lifecycle.
struct FR_API AttemptRecord {
  AttemptId id;
  Sha256Digest idempotency_key{};
  ScopeId scope;
  SubjectId subject;
  ActionKind action{ActionKind::None};
  CoordinatorEpoch epoch;
  BootId boot;
  AuthorityVector authority;
  AttemptState state{AttemptState::Invalid};
  UnixMillis issued_unix_ms{0};
  std::vector<AttemptTransition> transitions;

  [[nodiscard]] bool terminal() const noexcept { return IsTerminalAttemptState(state); }
};

/// A committed reconciliation outcome. This is what survives a restart as
/// fact, and it is explicitly labelled with how strong the claim is.
struct FR_API ReconciliationOutcome {
  Sha256Digest idempotency_key{};
  ScopeId scope;
  SubjectId subject;
  ActionKind action{ActionKind::None};
  AttemptId attempt;
  CoordinatorEpoch epoch;
  BootId boot;
  AttemptState terminal_state{AttemptState::Invalid};
  ReasonCode reason{ReasonCode::None};
  UnixMillis committed_unix_ms{0};
  /// True only when independent verification evidence was accepted.
  bool verified{false};
  ObservationGeneration verified_generation;
  /// True when the runtime knows an effect may have reached the fabric but the
  /// outcome was never confirmed.
  bool ambiguous{false};

  friend bool operator==(const ReconciliationOutcome&, const ReconciliationOutcome&) = default;
};

/// Verification evidence for an applied attempt.
struct FR_API VerificationEvidence {
  ScopeId scope;
  SubjectId subject;
  Sha256Digest idempotency_key{};
  ObservationId observation;
  ObservationGeneration generation;
  CoordinatorEpoch received_epoch;
  ReporterId verifier;
  /// The reporter whose application is being verified. Must differ from
  /// verifier when policy requires independent verification.
  ReporterId applier;
  UnixMillis received_unix_ms{0};
  bool confirms_effect{false};

  friend bool operator==(const VerificationEvidence&, const VerificationEvidence&) = default;
};

/// A fence. Fences are durable and they revoke authority; they never grant it.
struct FR_API FenceEntry {
  FenceId id;
  ScopeId scope;
  /// Empty means the fence covers the whole scope.
  SubjectId subject;
  /// Every attempt issued under an epoch strictly below this value is fenced.
  CoordinatorEpoch fenced_below;
  ReasonCode reason{ReasonCode::None};
  std::string detail;
  UnixMillis created_unix_ms{0};

  [[nodiscard]] bool scope_wide() const noexcept { return subject.empty(); }

  friend bool operator==(const FenceEntry&, const FenceEntry&) = default;
};

[[nodiscard]] FR_API std::string EncodeFence(const FenceEntry& fence);
[[nodiscard]] FR_API bool DecodeFence(CanonicalReader& reader, FenceEntry& out);
[[nodiscard]] FR_API std::string EncodeOutcome(const ReconciliationOutcome& outcome);
[[nodiscard]] FR_API bool DecodeOutcome(CanonicalReader& reader, ReconciliationOutcome& out);

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_LINEAGE_HPP
