// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded resource envelope.
//
// Every table, history, queue, document and explanation in the runtime is
// bounded. Exceeding a bound produces a deterministic refusal, never
// unbounded growth and never process instability.

#ifndef SUMMON_FABRIC_RECONCILIATION_LIMITS_HPP
#define SUMMON_FABRIC_RECONCILIATION_LIMITS_HPP

#include <cstddef>
#include <cstdint>

#include "summon/fabric_reconciliation/platform.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Which kind of evidence produced a document, decision or plan.
///
/// The label is carried all the way into plans and explanations so that a
/// reader can never mistake a fixture for physical hardware.
enum class EvidenceClass : std::uint8_t {
  Unknown = 0,
  /// Produced by, or about, real network elements actually exercised.
  Real = 1,
  /// Produced by deterministic synthetic fixtures. No hardware involved.
  Synthetic = 2,
  /// The runtime cannot express this quantity at all.
  Unsupported = 3,
};

[[nodiscard]] FR_API const char* ToText(EvidenceClass value) noexcept;
[[nodiscard]] FR_API bool TryParseEvidenceClass(const char* text, EvidenceClass& out) noexcept;

/// Hard resource envelope for one runtime incarnation.
struct FR_API RuntimeLimits {
  /// Maximum distinct scopes tracked by one store.
  std::size_t max_scopes = 4096;
  /// Maximum subjects retained per scope by a committed intent.
  std::size_t max_subjects_per_scope = 4000000;
  /// Maximum attributes compared per subject.
  std::size_t max_attributes_per_subject = 256;
  /// Maximum subjects accepted in one submitted document.
  std::size_t max_subjects_per_document = 4000000;
  /// Maximum size of a journal record payload, checked before allocation.
  std::size_t max_journal_payload_bytes = 8u * 1024u * 1024u;
  /// Maximum size of a journal file the runtime will map into memory.
  std::size_t max_journal_bytes = 512u * 1024u * 1024u;
  /// Retained observation evidence generations per scope.
  std::size_t max_retained_observations_per_scope = 8;
  /// Retained attempt records, oldest terminal attempts evicted first.
  std::size_t max_retained_attempts = 16384;
  /// Retained committed outcome records.
  std::size_t max_retained_outcomes = 16384;
  /// Retained lifecycle transitions per attempt.
  std::size_t max_attempt_transitions = 16;
  /// Maximum actions in one reconciliation plan.
  std::size_t max_plan_actions = 262144;
  /// Maximum ordered explanation steps attached to one decision.
  std::size_t max_explanation_steps = 32;
  /// Maximum concurrent transport sessions served by one runtime.
  std::size_t max_sessions = 256;
  /// Maximum frames a single session may have in flight before it is refused.
  std::size_t max_in_flight_frames_per_session = 8;
  /// Maximum byte length of one wire payload, checked before allocation.
  std::size_t max_wire_payload_bytes = 16u * 1024u * 1024u;
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_LIMITS_HPP
