// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Reconciliation policy: the durable rules that decide which differences are
// eligible to become reconciliation actions and what "fresh" means.
//
// Policy is fail-closed. Every gate defaults to the conservative value, and
// policy is never inferred from observations.

#ifndef SUMMON_FABRIC_RECONCILIATION_POLICY_HPP
#define SUMMON_FABRIC_RECONCILIATION_POLICY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// Freshness horizon and coverage rules.
struct FR_API FreshnessPolicy {
  /// Maximum accepted age of an observation, in milliseconds. Zero disables
  /// the temporal bound. The temporal bound is never sufficient on its own:
  /// see require_current_epoch.
  std::int64_t max_observation_age_ms{30000};
  /// When true, an observation recorded by a previous runtime incarnation is
  /// stale no matter how recent its timestamp is. Persistence is not liveness.
  /// This is the default and it is the behaviour the release proves.
  bool require_current_epoch{true};
  /// When true, absence of a subject or attribute may only be concluded from
  /// an observation that declares complete coverage and, for subjects, the
  /// right to assert absence.
  bool require_complete_coverage_for_absence{true};

  friend bool operator==(const FreshnessPolicy&, const FreshnessPolicy&) = default;
};

/// The complete reconciliation policy document.
struct FR_API ReconciliationPolicy {
  PolicyId id;
  PolicyVersion version;
  FreshnessPolicy freshness;

  /// Eligibility gates. A false gate never turns a difference into
  /// "already satisfied": it turns it into an explicitly blocked decision.
  bool permit_apply_missing{true};
  bool permit_apply_mismatched{true};
  bool permit_withdraw_unexpected{true};

  /// When true, the runtime refuses to treat a plan as convergence proof
  /// unless every subject in scope has complete coverage.
  bool require_complete_coverage_for_convergence_proof{true};

  /// When true, a verified outcome requires verification evidence produced by
  /// a reporter other than the one whose application is being verified.
  bool require_independent_verification{true};

  /// Attributes excluded from comparison. Sorted and de-duplicated on
  /// canonicalisation. Attributes listed here are reported with reason
  /// AttributeNotManagedByPolicy only when they are the sole difference.
  std::vector<AttributeKey> unmanaged_attributes;

  /// Attributes exempt from reconciliation actions even when they differ. A
  /// difference on such an attribute is reported and blocked, never silently
  /// ignored.
  std::vector<AttributeKey> observe_only_attributes;

  friend bool operator==(const ReconciliationPolicy&, const ReconciliationPolicy&) = default;
};

/// The conservative default policy: current epoch required, complete coverage
/// required for absence, independent verification required.
[[nodiscard]] FR_API ReconciliationPolicy DefaultPolicy();

/// Sorts and de-duplicates the attribute lists so that two policies that differ
/// only in list order have the same digest and the same behaviour.
[[nodiscard]] FR_API Status CanonicalisePolicy(ReconciliationPolicy& policy,
                                               const RuntimeLimits& limits);

[[nodiscard]] FR_API Sha256Digest ComputePolicyDigest(const ReconciliationPolicy& policy) noexcept;

[[nodiscard]] FR_API std::string EncodePolicy(const ReconciliationPolicy& policy);
[[nodiscard]] FR_API bool DecodePolicy(CanonicalReader& reader, ReconciliationPolicy& out,
                                       const RuntimeLimits& limits);
[[nodiscard]] FR_API bool DecodePolicyExact(std::string_view bytes, ReconciliationPolicy& out,
                                            const RuntimeLimits& limits);

[[nodiscard]] FR_API bool IsAttributeUnmanaged(const ReconciliationPolicy& policy,
                                               const AttributeKey& key) noexcept;
[[nodiscard]] FR_API bool IsAttributeObserveOnly(const ReconciliationPolicy& policy,
                                                 const AttributeKey& key) noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_POLICY_HPP
