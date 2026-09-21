// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Authority vectors.
//
// Every externally visible decision names the exact authority-bearing
// dependencies that made it legal: coordinator epoch, process incarnation,
// policy identity and digest, definition, intent generation and digest, and
// the exact observation evidence. When any of those change, the decision is
// revocable and the runtime fences it rather than quietly reusing it.

#ifndef SUMMON_FABRIC_RECONCILIATION_AUTHORITY_HPP
#define SUMMON_FABRIC_RECONCILIATION_AUTHORITY_HPP

#include <cstdint>
#include <string>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Bit mask naming the authority components that failed to bind. A mask is
/// used rather than a list so that an assessment is bounded and comparable.
enum class AuthorityComponent : std::uint32_t {
  None = 0u,
  CoordinatorEpoch = 1u << 0,
  BootIdentity = 1u << 1,
  PolicyId = 1u << 2,
  PolicyVersion = 1u << 3,
  PolicyDigest = 1u << 4,
  Definition = 1u << 5,
  IntentGeneration = 1u << 6,
  IntentDigest = 1u << 7,
  ObservationIdentity = 1u << 8,
  ObservationGeneration = 1u << 9,
  ObservationReporter = 1u << 10,
  ObservationDigest = 1u << 11,
  ObservationEpoch = 1u << 12,
  ObservationFreshness = 1u << 13,
  Fence = 1u << 14,
};

using AuthorityMask = std::uint32_t;

[[nodiscard]] constexpr AuthorityMask MaskOf(AuthorityComponent component) noexcept {
  return static_cast<AuthorityMask>(component);
}

[[nodiscard]] constexpr AuthorityMask Combine(AuthorityMask lhs, AuthorityMask rhs) noexcept {
  return lhs | rhs;
}

/// Human-readable name of one component, for explain output.
[[nodiscard]] FR_API const char* ToText(AuthorityComponent component) noexcept;

/// Deterministic rendering of a mask as a comma-separated component list in
/// ascending bit order. Bounded by the number of components.
[[nodiscard]] FR_API std::string RenderAuthorityMask(AuthorityMask mask);

/// The complete set of authority-bearing bindings behind one decision.
struct FR_API AuthorityVector {
  CoordinatorEpoch coordinator_epoch;
  BootId boot;

  PolicyId policy_id;
  PolicyVersion policy_version;
  Sha256Digest policy_digest{};

  DefinitionId definition;

  IntentGeneration intent_generation;
  IntentId intent;
  Sha256Digest intent_digest{};

  ObservationId observation;
  ObservationGeneration observation_generation;
  ReporterId observation_reporter;
  Sha256Digest observation_digest{};
  CoordinatorEpoch observation_epoch;
  UnixMillis observation_received_unix_ms{0};

  /// Canonical encoding of the whole vector, used for digests and durable
  /// records. Component order is fixed and documented in canonical.cpp.
  void Encode(CanonicalWriter& writer) const noexcept;
  [[nodiscard]] static bool Decode(CanonicalReader& reader, AuthorityVector& out);

  [[nodiscard]] Sha256Digest Digest() const noexcept;
  [[nodiscard]] std::string Render() const;
};

/// Result of comparing an authority vector against live authority.
struct FR_API AuthorityAssessment {
  bool proven{false};
  ReasonCode reason{ReasonCode::AuthorityEpochMismatch};
  AuthorityMask failed_components{0};

  [[nodiscard]] std::string RenderMask() const { return RenderAuthorityMask(failed_components); }
};

/// Live authority that a vector must bind against.
struct FR_API LiveAuthority {
  CoordinatorEpoch coordinator_epoch;
  BootId boot;
  PolicyId policy_id;
  PolicyVersion policy_version;
  Sha256Digest policy_digest{};
};

/// Evaluates an authority vector against live authority.
///
/// Fail-closed: an unknown or unset component never counts as a match, and the
/// caller-supplied freshness verdict must be Fresh for authority to be proven.
[[nodiscard]] FR_API AuthorityAssessment AssessAuthority(const AuthorityVector& vector,
                                                         const LiveAuthority& live,
                                                         ReasonCode freshness_reason,
                                                         bool freshness_ok,
                                                         bool fence_holds) noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_AUTHORITY_HPP
