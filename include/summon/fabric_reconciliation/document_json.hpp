// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Human-readable document surface.
//
// The durable and wire formats are canonical binary. This module is the
// human-facing counterpart used by the command line tools, by fixtures and by
// tests. It is a total, strict reader: a malformed document produces a
// Rejected status and never a partially populated domain object.

#ifndef SUMMON_FABRIC_RECONCILIATION_DOCUMENT_JSON_HPP
#define SUMMON_FABRIC_RECONCILIATION_DOCUMENT_JSON_HPP

#include <string>
#include <string_view>

#include "summon/fabric_reconciliation/document.hpp"
#include "summon/fabric_reconciliation/engine.hpp"
#include "summon/fabric_reconciliation/lineage.hpp"
#include "summon/fabric_reconciliation/plan.hpp"
#include "summon/fabric_reconciliation/policy.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Renders a domain object as canonical JSON: object keys are sorted, so two
/// equal objects always render to identical bytes.
[[nodiscard]] FR_API std::string ToJson(const IntentDocument& document);
[[nodiscard]] FR_API std::string ToJson(const ObservationSubmission& submission);
[[nodiscard]] FR_API std::string ToJson(const ObservationDocument& document);
[[nodiscard]] FR_API std::string ToJson(const ReconciliationPolicy& policy);
[[nodiscard]] FR_API std::string ToJson(const ReconciliationPlan& plan);
[[nodiscard]] FR_API std::string ToJson(const ClassificationReport& report);
[[nodiscard]] FR_API std::string ToJson(const BootReport& report);
[[nodiscard]] FR_API std::string ToJson(const ReconciliationOutcome& outcome);
[[nodiscard]] FR_API std::string ToJson(const FenceEntry& fence);
[[nodiscard]] FR_API std::string ToJson(const AttemptRecord& attempt);

/// Parses a domain object. Unknown members are refused rather than ignored so
/// that a typo in a fixture cannot silently change what is being tested.
[[nodiscard]] FR_API Status ParseJson(std::string_view text, IntentDocument& out,
                                      const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ParseJson(std::string_view text, ObservationSubmission& out,
                                      const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ParseJson(std::string_view text, ReconciliationPolicy& out,
                                      const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ParseJson(std::string_view text, FenceEntry& out,
                                      const RuntimeLimits& limits);
[[nodiscard]] FR_API Status ParseJson(std::string_view text, VerificationEvidence& out,
                                      const RuntimeLimits& limits);

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_DOCUMENT_JSON_HPP
