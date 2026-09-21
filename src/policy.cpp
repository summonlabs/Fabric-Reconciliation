// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/policy.hpp"

#include <algorithm>

#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

ReconciliationPolicy DefaultPolicy() {
  ReconciliationPolicy policy;
  const auto id = PolicyId::TryParse("default/fail-closed");
  if (id.has_value()) {
    policy.id = *id;
  }
  policy.version = PolicyVersion(1);
  policy.freshness.max_observation_age_ms = 30000;
  policy.freshness.require_current_epoch = true;
  policy.freshness.require_complete_coverage_for_absence = true;
  policy.permit_apply_missing = true;
  policy.permit_apply_mismatched = true;
  policy.permit_withdraw_unexpected = true;
  policy.require_complete_coverage_for_convergence_proof = true;
  policy.require_independent_verification = true;
  return policy;
}

Status CanonicalisePolicy(ReconciliationPolicy& policy, const RuntimeLimits& limits) {
  if (policy.id.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput, "policy id must not be empty");
  }
  if (policy.freshness.max_observation_age_ms < 0) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "max_observation_age_ms must not be negative");
  }
  const std::size_t bound = limits.max_attributes_per_subject * 16u;
  if (policy.unmanaged_attributes.size() > bound ||
      policy.observe_only_attributes.size() > bound) {
    return Status(StatusCode::LimitExceeded, ReasonCode::PlanActionLimitReached,
                  "policy attribute lists exceed the configured bound");
  }
  auto sort_unique = [](std::vector<AttributeKey>& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  };
  sort_unique(policy.unmanaged_attributes);
  sort_unique(policy.observe_only_attributes);
  for (const AttributeKey& key : policy.unmanaged_attributes) {
    if (key.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "policy unmanaged attribute key must not be empty");
    }
  }
  for (const AttributeKey& key : policy.observe_only_attributes) {
    if (key.empty()) {
      return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                    "policy observe-only attribute key must not be empty");
    }
  }
  return Status::Ok();
}

Sha256Digest ComputePolicyDigest(const ReconciliationPolicy& policy) noexcept {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(policy.id.str());
  writer.PutU64(policy.version.value());
  writer.PutI64(policy.freshness.max_observation_age_ms);
  writer.PutBool(policy.freshness.require_current_epoch);
  writer.PutBool(policy.freshness.require_complete_coverage_for_absence);
  writer.PutBool(policy.permit_apply_missing);
  writer.PutBool(policy.permit_apply_mismatched);
  writer.PutBool(policy.permit_withdraw_unexpected);
  writer.PutBool(policy.require_complete_coverage_for_convergence_proof);
  writer.PutBool(policy.require_independent_verification);
  writer.PutU32(static_cast<std::uint32_t>(policy.unmanaged_attributes.size() & 0xffffffffu));
  for (const AttributeKey& key : policy.unmanaged_attributes) {
    writer.PutBytes(key.str());
  }
  writer.PutU32(static_cast<std::uint32_t>(policy.observe_only_attributes.size() & 0xffffffffu));
  for (const AttributeKey& key : policy.observe_only_attributes) {
    writer.PutBytes(key.str());
  }
  return writer.Digest();
}

std::string EncodePolicy(const ReconciliationPolicy& policy) {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutBytes(policy.id.str());
  writer.PutU64(policy.version.value());
  writer.PutI64(policy.freshness.max_observation_age_ms);
  writer.PutBool(policy.freshness.require_current_epoch);
  writer.PutBool(policy.freshness.require_complete_coverage_for_absence);
  writer.PutBool(policy.permit_apply_missing);
  writer.PutBool(policy.permit_apply_mismatched);
  writer.PutBool(policy.permit_withdraw_unexpected);
  writer.PutBool(policy.require_complete_coverage_for_convergence_proof);
  writer.PutBool(policy.require_independent_verification);
  writer.PutU32(static_cast<std::uint32_t>(policy.unmanaged_attributes.size() & 0xffffffffu));
  for (const AttributeKey& key : policy.unmanaged_attributes) {
    writer.PutBytes(key.str());
  }
  writer.PutU32(static_cast<std::uint32_t>(policy.observe_only_attributes.size() & 0xffffffffu));
  for (const AttributeKey& key : policy.observe_only_attributes) {
    writer.PutBytes(key.str());
  }
  // A standalone policy payload carries its own integrity digest so that a
  // tampered document is refused even outside a journal record.
  writer.PutDigest(ComputePolicyDigest(policy));
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

bool DecodePolicy(CanonicalReader& reader, ReconciliationPolicy& out, const RuntimeLimits& limits) {
  std::uint16_t schema = 0;
  std::string id_text;
  std::uint64_t version = 0;
  std::int64_t max_age = 0;
  bool require_epoch = false;
  bool require_coverage = false;
  bool apply_missing = false;
  bool apply_mismatched = false;
  bool withdraw = false;
  bool convergence_proof = false;
  bool independent = false;
  if (!reader.ReadU16(schema) || schema != kCanonicalSchemaVersion) {
    return false;
  }
  if (!reader.ReadBytes(id_text) || !reader.ReadU64(version) || !reader.ReadI64(max_age) ||
      !reader.ReadBool(require_epoch) || !reader.ReadBool(require_coverage) ||
      !reader.ReadBool(apply_missing) || !reader.ReadBool(apply_mismatched) ||
      !reader.ReadBool(withdraw) || !reader.ReadBool(convergence_proof) ||
      !reader.ReadBool(independent)) {
    return false;
  }
  const auto id = PolicyId::TryParse(id_text);
  if (!id.has_value()) {
    return false;
  }
  ReconciliationPolicy policy;
  policy.id = *id;
  policy.version = PolicyVersion(version);
  policy.freshness.max_observation_age_ms = max_age;
  policy.freshness.require_current_epoch = require_epoch;
  policy.freshness.require_complete_coverage_for_absence = require_coverage;
  policy.permit_apply_missing = apply_missing;
  policy.permit_apply_mismatched = apply_mismatched;
  policy.permit_withdraw_unexpected = withdraw;
  policy.require_complete_coverage_for_convergence_proof = convergence_proof;
  policy.require_independent_verification = independent;

  const std::size_t bound = limits.max_attributes_per_subject * 16u;
  std::uint32_t unmanaged_count = 0;
  if (!reader.ReadU32(unmanaged_count) || static_cast<std::size_t>(unmanaged_count) > bound) {
    return false;
  }
  for (std::uint32_t index = 0; index < unmanaged_count; ++index) {
    std::string key_text;
    if (!reader.ReadBytes(key_text)) {
      return false;
    }
    const auto key = AttributeKey::TryParse(key_text);
    if (!key.has_value()) {
      return false;
    }
    policy.unmanaged_attributes.push_back(*key);
  }
  std::uint32_t observe_only_count = 0;
  if (!reader.ReadU32(observe_only_count) ||
      static_cast<std::size_t>(observe_only_count) > bound) {
    return false;
  }
  for (std::uint32_t index = 0; index < observe_only_count; ++index) {
    std::string key_text;
    if (!reader.ReadBytes(key_text)) {
      return false;
    }
    const auto key = AttributeKey::TryParse(key_text);
    if (!key.has_value()) {
      return false;
    }
    policy.observe_only_attributes.push_back(*key);
  }
  const Status status = CanonicalisePolicy(policy, limits);
  if (!status.ok()) {
    return false;
  }
  Sha256Digest digest{};
  if (!reader.ReadDigest(digest)) {
    return false;
  }
  if (!(digest == ComputePolicyDigest(policy))) {
    return false;
  }
  out = std::move(policy);
  return true;
}

bool DecodePolicyExact(std::string_view bytes, ReconciliationPolicy& out,
                       const RuntimeLimits& limits) {
  CanonicalReader reader(bytes);
  if (!DecodePolicy(reader, out, limits)) {
    return false;
  }
  return reader.AtEnd();
}

bool IsAttributeUnmanaged(const ReconciliationPolicy& policy, const AttributeKey& key) noexcept {
  return std::binary_search(policy.unmanaged_attributes.begin(),
                            policy.unmanaged_attributes.end(), key);
}

bool IsAttributeObserveOnly(const ReconciliationPolicy& policy, const AttributeKey& key) noexcept {
  return std::binary_search(policy.observe_only_attributes.begin(),
                            policy.observe_only_attributes.end(), key);
}

}  // namespace fabric_reconciliation
}  // namespace summon
