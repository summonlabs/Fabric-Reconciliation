// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Intended and observed fabric state documents.
//
// Intended state is a *declaration of desire*. It is durable and it is
// authority for what should be true, but it is never proof that anything was
// applied. Observed state is *evidence*. It is never authority, and its
// freshness is a property of the live process, not of the record.

#ifndef SUMMON_FABRIC_RECONCILIATION_DOCUMENT_HPP
#define SUMMON_FABRIC_RECONCILIATION_DOCUMENT_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/value.hpp"

namespace summon {
namespace fabric_reconciliation {

/// One subject inside an intent document.
struct FR_API SubjectIntent {
  SubjectId id;
  /// True when the intent enumerates every attribute that matters for this
  /// subject. When false, an attribute absent from the intent is unknown
  /// rather than unexpected.
  bool complete_attributes{true};
  SubjectState desired;

  friend bool operator==(const SubjectIntent&, const SubjectIntent&) = default;
};

/// One subject inside an observation document.
struct FR_API SubjectObservation {
  SubjectId id;
  /// True when the reporter saw the whole attribute set. When false, an
  /// attribute missing from the observation is a coverage gap, not an absence.
  bool complete_attributes{true};
  SubjectState observed;

  friend bool operator==(const SubjectObservation&, const SubjectObservation&) = default;
};

/// A submitted intent. Durable authority for "what should be true".
struct FR_API IntentDocument {
  IntentId intent;
  ScopeId scope;
  DefinitionId definition;
  IntentGeneration generation;
  PolicyVersion policy_version;
  EvidenceClass evidence{EvidenceClass::Unknown};
  /// True when this document enumerates every subject that should exist in the
  /// scope. When false, a subject present only in observation is unknown
  /// rather than unexpected.
  bool complete{true};
  std::map<SubjectId, SubjectIntent> subjects;
  /// Digest over the canonical encoding of everything above except this field.
  Sha256Digest digest{};

  friend bool operator==(const IntentDocument&, const IntentDocument&) = default;
};

/// An observation as submitted by a reporter.
///
/// The submitter cannot choose received_epoch or received_unix_ms: the runtime
/// stamps receipt itself. This is the mechanism that prevents a reporter from
/// claiming foreign or historical freshness.
struct FR_API ObservationSubmission {
  ObservationId observation;
  ScopeId scope;
  ReporterId reporter;
  ObservationGeneration generation;
  EvidenceClass evidence{EvidenceClass::Unknown};
  /// True when the reporter believes it covered the whole scope.
  bool complete{true};
  /// True when the reporter is entitled to assert that a subject or attribute
  /// is absent. Without it, absence is a coverage gap.
  bool authoritative_absence{false};
  std::map<SubjectId, SubjectObservation> subjects;

  friend bool operator==(const ObservationSubmission&, const ObservationSubmission&) = default;
};

/// An observation as recorded by the runtime. Durable evidence, never durable
/// authority.
struct FR_API ObservationDocument {
  ObservationId observation;
  ScopeId scope;
  ReporterId reporter;
  ObservationGeneration generation;
  EvidenceClass evidence{EvidenceClass::Unknown};
  bool complete{true};
  bool authoritative_absence{false};
  /// Epoch of the runtime incarnation that received this evidence.
  CoordinatorEpoch received_epoch;
  /// Host wall clock at receipt. Evidence, not a freshness proof on its own.
  UnixMillis received_unix_ms{0};
  std::map<SubjectId, SubjectObservation> subjects;
  Sha256Digest digest{};

  friend bool operator==(const ObservationDocument&, const ObservationDocument&) = default;
};

/// Canonical digest of an intent document's content.
[[nodiscard]] FR_API Sha256Digest ComputeIntentDigest(const IntentDocument& document) noexcept;

/// Canonical digest of an observation document's content, receipt stamps
/// included.
[[nodiscard]] FR_API Sha256Digest ComputeObservationDigest(const ObservationDocument& document) noexcept;

/// Validates structure, identity text, bounds and attribute ranges.
[[nodiscard]] FR_API Status ValidateIntent(const IntentDocument& document,
                                           const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ValidateObservation(const ObservationDocument& document,
                                                const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ValidateSubmission(const ObservationSubmission& submission,
                                               const RuntimeLimits& limits);

/// Canonical encodings, used by the journal, the wire protocol and digests.
[[nodiscard]] FR_API std::string EncodeIntent(const IntentDocument& document);
[[nodiscard]] FR_API std::string EncodeObservation(const ObservationDocument& document);
[[nodiscard]] FR_API std::string EncodeSubmission(const ObservationSubmission& submission);

[[nodiscard]] FR_API bool DecodeIntent(CanonicalReader& reader, IntentDocument& out,
                                       const RuntimeLimits& limits);
[[nodiscard]] FR_API bool DecodeObservation(CanonicalReader& reader, ObservationDocument& out,
                                            const RuntimeLimits& limits);
[[nodiscard]] FR_API bool DecodeSubmission(CanonicalReader& reader, ObservationSubmission& out,
                                           const RuntimeLimits& limits);

/// Decodes a whole buffer and requires that it is consumed exactly.
[[nodiscard]] FR_API bool DecodeIntentExact(std::string_view bytes, IntentDocument& out,
                                            const RuntimeLimits& limits);
[[nodiscard]] FR_API bool DecodeObservationExact(std::string_view bytes, ObservationDocument& out,
                                                 const RuntimeLimits& limits);
[[nodiscard]] FR_API bool DecodeSubmissionExact(std::string_view bytes,
                                                ObservationSubmission& out,
                                                const RuntimeLimits& limits);

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_DOCUMENT_HPP
