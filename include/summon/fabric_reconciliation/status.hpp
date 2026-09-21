// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Explicit outcome vocabulary.
//
// The runtime never maps an indeterminate, stale, conflicting, invalid or
// unsupported condition onto success or onto ordinary absence. Every such
// condition has its own enumerator and its own reason code.

#ifndef SUMMON_FABRIC_RECONCILIATION_STATUS_HPP
#define SUMMON_FABRIC_RECONCILIATION_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "summon/fabric_reconciliation/platform.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Coarse outcome of one externally visible operation.
enum class StatusCode : std::uint16_t {
  Ok = 0,
  /// Input was malformed, out of range, or refused by policy.
  Rejected = 1,
  /// The operation contradicts a committed fact (generation regression,
  /// contradictory duplicate, divergent re-publication).
  ConflictState = 2,
  /// The named thing is genuinely absent from the runtime.
  NotFound = 3,
  /// The authority vector the caller supplied does not match live authority.
  StaleAuthority = 4,
  /// A configured bound would be exceeded; the call was refused up front.
  LimitExceeded = 5,
  /// Durable state failed an integrity check. Never silently repaired.
  IntegrityFailure = 6,
  /// Outside the supported problem class.
  Unsupported = 7,
  /// The runtime cannot decide; the caller must not read this as success.
  Indeterminate = 8,
  /// A precondition that must hold did not hold.
  PreconditionFailed = 9,
  /// An internal invariant was violated. Always a defect.
  InternalError = 10,
};

[[nodiscard]] FR_API const char* ToText(StatusCode value) noexcept;

/// Deterministic machine-readable explanation code.
///
/// Numeric ranges group codes by concern so that a caller can branch on the
/// concern without enumerating every code:
///   100..199  drift classification
///   200..299  staleness, ordering and coverage
///   300..399  authority and fencing
///   400..499  action and policy eligibility
///   500..599  persistence and integrity
///   600..699  transport and session
///   700..799  lifecycle and attempts
enum class ReasonCode : std::uint16_t {
  None = 0,

  // ---- drift classification (100..199) ----
  ClassifiedAlreadyConverged = 100,
  ClassifiedSubjectMissing = 101,
  ClassifiedSubjectUnexpected = 102,
  ClassifiedAttributeMismatch = 103,
  ClassifiedAttributeMissing = 104,
  ClassifiedAttributeUnexpected = 105,
  ClassifiedAttributeNotComparable = 106,

  // ---- staleness, ordering, coverage (200..299) ----
  ObservationExpired = 200,
  ObservationForeignEpoch = 201,
  ObservationClockRegression = 202,
  ObservationGenerationRegressed = 203,
  IntentGenerationRegressed = 204,
  DuplicateObservationIdentical = 205,
  DuplicateIntentIdentical = 206,
  ConflictingRepublishers = 207,
  NoObservationForScope = 208,
  NoIntentForScope = 209,
  ObservationCoverageIncomplete = 220,
  IntentCoverageIncomplete = 221,
  ObservationCannotAssertAbsence = 222,
  AttributeCoverageIncomplete = 223,
  AttributeNotManagedByPolicy = 224,

  // ---- authority and fencing (300..399) ----
  AuthorityProven = 300,
  AuthorityEpochMismatch = 301,
  AuthorityBootMismatch = 302,
  AuthorityPolicyMismatch = 303,
  AuthorityIntentGenerationMismatch = 304,
  AuthorityObservationGenerationMismatch = 305,
  AuthorityDefinitionMismatch = 306,
  ScopeFenced = 310,
  SubjectFenced = 311,
  FenceEpochNotNewer = 312,
  AttemptInterruptedByRestart = 313,
  AttemptFencedByEpochAdvance = 314,

  // ---- action and policy eligibility (400..499) ----
  ActionEligible = 400,
  ActionNotRequired = 401,
  PolicyForbidsApplyMissing = 402,
  PolicyForbidsApplyMismatch = 403,
  PolicyForbidsWithdrawUnexpected = 404,
  DriftIsIndeterminate = 405,
  DriftIsConflicted = 406,
  SubjectKindUnsupported = 407,
  PlanActionLimitReached = 408,
  PlanTruncated = 409,
  UnsupportedInput = 410,
  MalformedPayload = 411,

  // ---- persistence and integrity (500..599) ----
  JournalHeaderInvalid = 500,
  JournalVersionUnsupported = 501,
  JournalRecordDigestMismatch = 502,
  JournalSequenceRegression = 503,
  JournalTornTailRecovered = 504,
  JournalTrailingGarbage = 505,
  JournalPayloadLengthInvalid = 506,
  JournalRecordTypeUnknown = 507,
  JournalEnumInvalid = 508,
  SnapshotDigestMismatch = 509,
  SnapshotStoreMismatch = 510,
  SnapshotAheadOfJournal = 511,
  DurableWriteFailed = 512,
  SnapshotReplacementFailed = 513,

  // ---- transport and session (600..699) ----
  FrameMagicInvalid = 600,
  FrameVersionUnsupported = 601,
  FrameTypeUnknown = 602,
  FramePayloadTooLarge = 603,
  FrameDigestMismatch = 604,
  FrameTruncated = 605,
  FrameTrailingBytes = 606,
  SessionUnknown = 607,
  SessionIdentityMismatch = 608,
  SessionEpochMismatch = 609,
  SessionBootMismatch = 610,
  SessionSequenceRegression = 611,
  SessionLimitReached = 612,
  SessionClosed = 613,

  // ---- lifecycle and attempts (700..799) ----
  AttemptIssued = 700,
  AttemptAcknowledged = 701,
  AttemptApplied = 702,
  AttemptVerified = 703,
  AttemptFailed = 704,
  AttemptAbandoned = 705,
  AttemptSuperseded = 706,
  AttemptTerminalAlready = 707,
  AttemptUnknown = 708,
  AttemptTransitionIllegal = 709,
  VerificationNotIndependent = 710,
  VerificationNotNewer = 711,
  ReconciliationOutcomeCommitted = 712,

  // ---- persistence semantics (800..899) ----
  RestartEpochAdvanced = 800,
  RestartFreshnessNotRestored = 801,
  RestartAttemptsFenced = 802,
  RestartLeasesNotRestored = 803,
};

[[nodiscard]] FR_API const char* ToText(ReasonCode value) noexcept;
[[nodiscard]] FR_API bool IsDriftReason(ReasonCode value) noexcept;
[[nodiscard]] FR_API bool IsAuthorityReason(ReasonCode value) noexcept;

/// Reasonable, copyable status object. The detail string is bounded by the
/// caller that produces it; no runtime path builds an unbounded detail.
class FR_API Status {
 public:
  Status() = default;
  Status(StatusCode code, ReasonCode reason) : code_(code), reason_(reason) {}
  Status(StatusCode code, ReasonCode reason, std::string detail)
      : code_(code), reason_(reason), detail_(std::move(detail)) {}

  [[nodiscard]] static Status Ok() noexcept { return Status(); }
  [[nodiscard]] static Status Ok(ReasonCode reason) noexcept { return Status(StatusCode::Ok, reason); }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] ReasonCode reason() const noexcept { return reason_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// One-line deterministic rendering, used by the CLI and by explain output.
  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const Status&, const Status&) = default;

 private:
  StatusCode code_{StatusCode::Ok};
  ReasonCode reason_{ReasonCode::None};
  std::string detail_;
};

/// Minimal result carrier. A failed result never carries a value, so a caller
/// cannot accidentally read a default-constructed object as a success.
template <typename T>
class Result {
 public:
  Result(Status status) : status_(std::move(status)) {}
  Result(T value) : status_(Status::Ok()), value_(std::move(value)) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok() && value_.has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }
  [[nodiscard]] ReasonCode reason() const noexcept { return status_.reason(); }
  [[nodiscard]] std::string ToString() const { return status_.ToString(); }

  /// Precondition: ok(). Calling on a failed result is a programming error and
  /// terminates rather than returning a fabricated value.
  [[nodiscard]] const T& value() const { return *value_; }
  [[nodiscard]] T& value() { return *value_; }
  [[nodiscard]] const T* operator->() const { return &*value_; }
  [[nodiscard]] T* operator->() { return &*value_; }
  [[nodiscard]] const T& operator*() const { return *value_; }
  [[nodiscard]] T& operator*() { return *value_; }

 private:
  Status status_;
  std::optional<T> value_;
};

using VoidResult = Result<bool>;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_STATUS_HPP
