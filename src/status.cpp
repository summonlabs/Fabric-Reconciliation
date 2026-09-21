// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/status.hpp"

#include "summon/fabric_reconciliation/limits.hpp"

namespace summon {
namespace fabric_reconciliation {

const char* ToText(StatusCode value) noexcept {
  switch (value) {
    case StatusCode::Ok: return "OK";
    case StatusCode::Rejected: return "REJECTED";
    case StatusCode::ConflictState: return "CONFLICT";
    case StatusCode::NotFound: return "NOT_FOUND";
    case StatusCode::StaleAuthority: return "STALE_AUTHORITY";
    case StatusCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case StatusCode::IntegrityFailure: return "INTEGRITY_FAILURE";
    case StatusCode::Unsupported: return "UNSUPPORTED";
    case StatusCode::Indeterminate: return "INDETERMINATE";
    case StatusCode::PreconditionFailed: return "PRECONDITION_FAILED";
    case StatusCode::InternalError: return "INTERNAL_ERROR";
  }
  return "INVALID";
}

const char* ToText(ReasonCode value) noexcept {
  switch (value) {
    case ReasonCode::None: return "NONE";

    case ReasonCode::ClassifiedAlreadyConverged: return "ALREADY_CONVERGED";
    case ReasonCode::ClassifiedSubjectMissing: return "SUBJECT_MISSING";
    case ReasonCode::ClassifiedSubjectUnexpected: return "SUBJECT_UNEXPECTED";
    case ReasonCode::ClassifiedAttributeMismatch: return "ATTRIBUTE_MISMATCH";
    case ReasonCode::ClassifiedAttributeMissing: return "ATTRIBUTE_MISSING";
    case ReasonCode::ClassifiedAttributeUnexpected: return "ATTRIBUTE_UNEXPECTED";
    case ReasonCode::ClassifiedAttributeNotComparable: return "ATTRIBUTE_NOT_COMPARABLE";

    case ReasonCode::ObservationExpired: return "OBSERVATION_EXPIRED";
    case ReasonCode::ObservationForeignEpoch: return "OBSERVATION_FOREIGN_EPOCH";
    case ReasonCode::ObservationClockRegression: return "OBSERVATION_CLOCK_REGRESSION";
    case ReasonCode::ObservationGenerationRegressed: return "OBSERVATION_GENERATION_REGRESSED";
    case ReasonCode::IntentGenerationRegressed: return "INTENT_GENERATION_REGRESSED";
    case ReasonCode::DuplicateObservationIdentical: return "DUPLICATE_OBSERVATION_IDENTICAL";
    case ReasonCode::DuplicateIntentIdentical: return "DUPLICATE_INTENT_IDENTICAL";
    case ReasonCode::ConflictingRepublishers: return "CONFLICTING_REPUBLISHERS";
    case ReasonCode::NoObservationForScope: return "NO_OBSERVATION_FOR_SCOPE";
    case ReasonCode::NoIntentForScope: return "NO_INTENT_FOR_SCOPE";
    case ReasonCode::ObservationCoverageIncomplete: return "OBSERVATION_COVERAGE_INCOMPLETE";
    case ReasonCode::IntentCoverageIncomplete: return "INTENT_COVERAGE_INCOMPLETE";
    case ReasonCode::ObservationCannotAssertAbsence: return "OBSERVATION_CANNOT_ASSERT_ABSENCE";
    case ReasonCode::AttributeCoverageIncomplete: return "ATTRIBUTE_COVERAGE_INCOMPLETE";
    case ReasonCode::AttributeNotManagedByPolicy: return "ATTRIBUTE_NOT_MANAGED_BY_POLICY";

    case ReasonCode::AuthorityProven: return "AUTHORITY_PROVEN";
    case ReasonCode::AuthorityEpochMismatch: return "AUTHORITY_EPOCH_MISMATCH";
    case ReasonCode::AuthorityBootMismatch: return "AUTHORITY_BOOT_MISMATCH";
    case ReasonCode::AuthorityPolicyMismatch: return "AUTHORITY_POLICY_MISMATCH";
    case ReasonCode::AuthorityIntentGenerationMismatch: return "AUTHORITY_INTENT_GENERATION_MISMATCH";
    case ReasonCode::AuthorityObservationGenerationMismatch:
      return "AUTHORITY_OBSERVATION_GENERATION_MISMATCH";
    case ReasonCode::AuthorityDefinitionMismatch: return "AUTHORITY_DEFINITION_MISMATCH";
    case ReasonCode::ScopeFenced: return "SCOPE_FENCED";
    case ReasonCode::SubjectFenced: return "SUBJECT_FENCED";
    case ReasonCode::FenceEpochNotNewer: return "FENCE_EPOCH_NOT_NEWER";
    case ReasonCode::AttemptInterruptedByRestart: return "ATTEMPT_INTERRUPTED_BY_RESTART";
    case ReasonCode::AttemptFencedByEpochAdvance: return "ATTEMPT_FENCED_BY_EPOCH_ADVANCE";

    case ReasonCode::ActionEligible: return "ACTION_ELIGIBLE";
    case ReasonCode::ActionNotRequired: return "ACTION_NOT_REQUIRED";
    case ReasonCode::PolicyForbidsApplyMissing: return "POLICY_FORBIDS_APPLY_MISSING";
    case ReasonCode::PolicyForbidsApplyMismatch: return "POLICY_FORBIDS_APPLY_MISMATCH";
    case ReasonCode::PolicyForbidsWithdrawUnexpected: return "POLICY_FORBIDS_WITHDRAW_UNEXPECTED";
    case ReasonCode::DriftIsIndeterminate: return "DRIFT_IS_INDETERMINATE";
    case ReasonCode::DriftIsConflicted: return "DRIFT_IS_CONFLICTED";
    case ReasonCode::SubjectKindUnsupported: return "SUBJECT_KIND_UNSUPPORTED";
    case ReasonCode::PlanActionLimitReached: return "PLAN_ACTION_LIMIT_REACHED";
    case ReasonCode::PlanTruncated: return "PLAN_TRUNCATED";
    case ReasonCode::UnsupportedInput: return "UNSUPPORTED_INPUT";
    case ReasonCode::MalformedPayload: return "MALFORMED_PAYLOAD";

    case ReasonCode::JournalHeaderInvalid: return "JOURNAL_HEADER_INVALID";
    case ReasonCode::JournalVersionUnsupported: return "JOURNAL_VERSION_UNSUPPORTED";
    case ReasonCode::JournalRecordDigestMismatch: return "JOURNAL_RECORD_DIGEST_MISMATCH";
    case ReasonCode::JournalSequenceRegression: return "JOURNAL_SEQUENCE_REGRESSION";
    case ReasonCode::JournalTornTailRecovered: return "JOURNAL_TORN_TAIL_RECOVERED";
    case ReasonCode::JournalTrailingGarbage: return "JOURNAL_TRAILING_GARBAGE";
    case ReasonCode::JournalPayloadLengthInvalid: return "JOURNAL_PAYLOAD_LENGTH_INVALID";
    case ReasonCode::JournalRecordTypeUnknown: return "JOURNAL_RECORD_TYPE_UNKNOWN";
    case ReasonCode::JournalEnumInvalid: return "JOURNAL_ENUM_INVALID";
    case ReasonCode::SnapshotDigestMismatch: return "SNAPSHOT_DIGEST_MISMATCH";
    case ReasonCode::SnapshotStoreMismatch: return "SNAPSHOT_STORE_MISMATCH";
    case ReasonCode::SnapshotAheadOfJournal: return "SNAPSHOT_AHEAD_OF_JOURNAL";
    case ReasonCode::DurableWriteFailed: return "DURABLE_WRITE_FAILED";
    case ReasonCode::SnapshotReplacementFailed: return "SNAPSHOT_REPLACEMENT_FAILED";

    case ReasonCode::FrameMagicInvalid: return "FRAME_MAGIC_INVALID";
    case ReasonCode::FrameVersionUnsupported: return "FRAME_VERSION_UNSUPPORTED";
    case ReasonCode::FrameTypeUnknown: return "FRAME_TYPE_UNKNOWN";
    case ReasonCode::FramePayloadTooLarge: return "FRAME_PAYLOAD_TOO_LARGE";
    case ReasonCode::FrameDigestMismatch: return "FRAME_DIGEST_MISMATCH";
    case ReasonCode::FrameTruncated: return "FRAME_TRUNCATED";
    case ReasonCode::FrameTrailingBytes: return "FRAME_TRAILING_BYTES";
    case ReasonCode::SessionUnknown: return "SESSION_UNKNOWN";
    case ReasonCode::SessionIdentityMismatch: return "SESSION_IDENTITY_MISMATCH";
    case ReasonCode::SessionEpochMismatch: return "SESSION_EPOCH_MISMATCH";
    case ReasonCode::SessionBootMismatch: return "SESSION_BOOT_MISMATCH";
    case ReasonCode::SessionSequenceRegression: return "SESSION_SEQUENCE_REGRESSION";
    case ReasonCode::SessionLimitReached: return "SESSION_LIMIT_REACHED";
    case ReasonCode::SessionClosed: return "SESSION_CLOSED";

    case ReasonCode::AttemptIssued: return "ATTEMPT_ISSUED";
    case ReasonCode::AttemptAcknowledged: return "ATTEMPT_ACKNOWLEDGED";
    case ReasonCode::AttemptApplied: return "ATTEMPT_APPLIED";
    case ReasonCode::AttemptVerified: return "ATTEMPT_VERIFIED";
    case ReasonCode::AttemptFailed: return "ATTEMPT_FAILED";
    case ReasonCode::AttemptAbandoned: return "ATTEMPT_ABANDONED";
    case ReasonCode::AttemptSuperseded: return "ATTEMPT_SUPERSEDED";
    case ReasonCode::AttemptTerminalAlready: return "ATTEMPT_TERMINAL_ALREADY";
    case ReasonCode::AttemptUnknown: return "ATTEMPT_UNKNOWN";
    case ReasonCode::AttemptTransitionIllegal: return "ATTEMPT_TRANSITION_ILLEGAL";
    case ReasonCode::VerificationNotIndependent: return "VERIFICATION_NOT_INDEPENDENT";
    case ReasonCode::VerificationNotNewer: return "VERIFICATION_NOT_NEWER";
    case ReasonCode::ReconciliationOutcomeCommitted: return "RECONCILIATION_OUTCOME_COMMITTED";

    case ReasonCode::RestartEpochAdvanced: return "RESTART_EPOCH_ADVANCED";
    case ReasonCode::RestartFreshnessNotRestored: return "RESTART_FRESHNESS_NOT_RESTORED";
    case ReasonCode::RestartAttemptsFenced: return "RESTART_ATTEMPTS_FENCED";
    case ReasonCode::RestartLeasesNotRestored: return "RESTART_LEASES_NOT_RESTORED";
  }
  return "UNKNOWN_REASON";
}

bool IsDriftReason(ReasonCode value) noexcept {
  const auto raw = static_cast<std::uint16_t>(value);
  return raw >= 100 && raw < 200;
}

bool IsAuthorityReason(ReasonCode value) noexcept {
  const auto raw = static_cast<std::uint16_t>(value);
  return raw >= 300 && raw < 400;
}

std::string Status::ToString() const {
  std::string result = ToText(code_);
  result += "(";
  result += ToText(reason_);
  if (!detail_.empty()) {
    result += ": ";
    result += detail_;
  }
  result += ")";
  return result;
}

const char* ToText(EvidenceClass value) noexcept {
  switch (value) {
    case EvidenceClass::Unknown: return "UNKNOWN";
    case EvidenceClass::Real: return "REAL";
    case EvidenceClass::Synthetic: return "SYNTHETIC";
    case EvidenceClass::Unsupported: return "UNSUPPORTED";
  }
  return "INVALID";
}

bool TryParseEvidenceClass(const char* text, EvidenceClass& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::string_view view(text);
  if (view == "REAL") {
    out = EvidenceClass::Real;
    return true;
  }
  if (view == "SYNTHETIC") {
    out = EvidenceClass::Synthetic;
    return true;
  }
  if (view == "UNSUPPORTED") {
    out = EvidenceClass::Unsupported;
    return true;
  }
  if (view == "UNKNOWN") {
    out = EvidenceClass::Unknown;
    return true;
  }
  return false;
}

}  // namespace fabric_reconciliation
}  // namespace summon
