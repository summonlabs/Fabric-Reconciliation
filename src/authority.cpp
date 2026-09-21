// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/authority.hpp"

namespace summon {
namespace fabric_reconciliation {
namespace {

struct ComponentName {
  AuthorityComponent component;
  const char* text;
};

constexpr ComponentName kComponentNames[] = {
    {AuthorityComponent::CoordinatorEpoch, "coordinator-epoch"},
    {AuthorityComponent::BootIdentity, "boot-identity"},
    {AuthorityComponent::PolicyId, "policy-id"},
    {AuthorityComponent::PolicyVersion, "policy-version"},
    {AuthorityComponent::PolicyDigest, "policy-digest"},
    {AuthorityComponent::Definition, "definition"},
    {AuthorityComponent::IntentGeneration, "intent-generation"},
    {AuthorityComponent::IntentDigest, "intent-digest"},
    {AuthorityComponent::ObservationIdentity, "observation-identity"},
    {AuthorityComponent::ObservationGeneration, "observation-generation"},
    {AuthorityComponent::ObservationReporter, "observation-reporter"},
    {AuthorityComponent::ObservationDigest, "observation-digest"},
    {AuthorityComponent::ObservationEpoch, "observation-epoch"},
    {AuthorityComponent::ObservationFreshness, "observation-freshness"},
    {AuthorityComponent::Fence, "fence"},
};

}  // namespace

const char* ToText(AuthorityComponent component) noexcept {
  for (const ComponentName& entry : kComponentNames) {
    if (entry.component == component) {
      return entry.text;
    }
  }
  return "none";
}

std::string RenderAuthorityMask(AuthorityMask mask) {
  if (mask == 0) {
    return "none";
  }
  std::string result;
  for (const ComponentName& entry : kComponentNames) {
    if ((mask & MaskOf(entry.component)) == 0) {
      continue;
    }
    if (!result.empty()) {
      result += ",";
    }
    result += entry.text;
  }
  return result;
}

void AuthorityVector::Encode(CanonicalWriter& writer) const noexcept {
  writer.PutU64(coordinator_epoch.value());
  writer.PutBytes(std::string_view(reinterpret_cast<const char*>(boot.bytes().data()),
                                   boot.bytes().size()));
  writer.PutBytes(policy_id.str());
  writer.PutU64(policy_version.value());
  writer.PutDigest(policy_digest);
  writer.PutBytes(definition.str());
  writer.PutU64(intent_generation.value());
  writer.PutBytes(intent.str());
  writer.PutDigest(intent_digest);
  writer.PutBytes(observation.str());
  writer.PutU64(observation_generation.value());
  writer.PutBytes(observation_reporter.str());
  writer.PutDigest(observation_digest);
  writer.PutU64(observation_epoch.value());
  writer.PutI64(observation_received_unix_ms);
}

bool AuthorityVector::Decode(CanonicalReader& reader, AuthorityVector& out) {
  AuthorityVector vector;
  std::uint64_t coordinator_epoch = 0;
  std::string boot_text;
  std::string policy_id_text;
  std::uint64_t policy_version = 0;
  std::string definition_text;
  std::uint64_t intent_generation = 0;
  std::string intent_text;
  std::string observation_text;
  std::uint64_t observation_generation = 0;
  std::string reporter_text;
  std::uint64_t observation_epoch = 0;
  std::int64_t observation_received = 0;

  if (!reader.ReadU64(coordinator_epoch) || !reader.ReadBytes(boot_text)) {
    return false;
  }
  if (boot_text.size() != 16) {
    return false;
  }
  std::array<std::uint8_t, 16> boot_bytes{};
  for (std::size_t index = 0; index < 16; ++index) {
    boot_bytes[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(boot_text[index]));
  }
  if (!reader.ReadBytes(policy_id_text) || !reader.ReadU64(policy_version) ||
      !reader.ReadDigest(vector.policy_digest) || !reader.ReadBytes(definition_text) ||
      !reader.ReadU64(intent_generation) || !reader.ReadBytes(intent_text) ||
      !reader.ReadDigest(vector.intent_digest) || !reader.ReadBytes(observation_text) ||
      !reader.ReadU64(observation_generation) || !reader.ReadBytes(reporter_text) ||
      !reader.ReadDigest(vector.observation_digest) || !reader.ReadU64(observation_epoch) ||
      !reader.ReadI64(observation_received)) {
    return false;
  }
  // A component that names no evidence is carried as empty text. It is never
  // treated as a match: AssessAuthority rejects an empty binding outright.
  // Non-empty text must still be a well-formed identity.
  auto assign = [](const std::string& text, auto& target) {
    if (text.empty()) {
      return true;
    }
    const auto parsed = std::remove_reference_t<decltype(target)>::TryParse(text);
    if (!parsed.has_value()) {
      return false;
    }
    target = *parsed;
    return true;
  };
  if (!assign(policy_id_text, vector.policy_id) || !assign(definition_text, vector.definition) ||
      !assign(intent_text, vector.intent) || !assign(observation_text, vector.observation) ||
      !assign(reporter_text, vector.observation_reporter)) {
    return false;
  }
  vector.coordinator_epoch = CoordinatorEpoch(coordinator_epoch);
  vector.boot = BootId::FromBytes(boot_bytes);
  vector.policy_version = PolicyVersion(policy_version);
  vector.intent_generation = IntentGeneration(intent_generation);
  vector.observation_generation = ObservationGeneration(observation_generation);
  vector.observation_epoch = CoordinatorEpoch(observation_epoch);
  vector.observation_received_unix_ms = observation_received;
  out = std::move(vector);
  return true;
}

Sha256Digest AuthorityVector::Digest() const noexcept {
  CanonicalWriter writer;
  Encode(writer);
  return writer.Digest();
}

std::string AuthorityVector::Render() const {
  std::string result;
  result += "epoch=";
  result += std::to_string(coordinator_epoch.value());
  result += " boot=";
  result += boot.ToHex().substr(0, 8);
  result += " policy=";
  result += policy_id.str();
  result += "/";
  result += std::to_string(policy_version.value());
  result += " definition=";
  result += definition.str();
  result += " intent=";
  result += intent.str();
  result += "/g";
  result += std::to_string(intent_generation.value());
  result += " observation=";
  result += observation.str();
  result += "/g";
  result += std::to_string(observation_generation.value());
  result += "/";
  result += observation_reporter.str();
  return result;
}

AuthorityAssessment AssessAuthority(const AuthorityVector& vector, const LiveAuthority& live,
                                   ReasonCode freshness_reason, bool freshness_ok,
                                   bool fence_holds) noexcept {
  AuthorityAssessment assessment;
  AuthorityMask mask = 0;
  if (vector.coordinator_epoch != live.coordinator_epoch) {
    mask = Combine(mask, MaskOf(AuthorityComponent::CoordinatorEpoch));
  }
  if (!(vector.boot == live.boot)) {
    mask = Combine(mask, MaskOf(AuthorityComponent::BootIdentity));
  }
  if (vector.policy_id.empty() || !(vector.policy_id == live.policy_id)) {
    mask = Combine(mask, MaskOf(AuthorityComponent::PolicyId));
  }
  if (!(vector.policy_version == live.policy_version)) {
    mask = Combine(mask, MaskOf(AuthorityComponent::PolicyVersion));
  }
  if (!(vector.policy_digest == live.policy_digest)) {
    mask = Combine(mask, MaskOf(AuthorityComponent::PolicyDigest));
  }
  if (vector.definition.empty()) {
    mask = Combine(mask, MaskOf(AuthorityComponent::Definition));
  }
  if (vector.intent_generation.value() == 0) {
    mask = Combine(mask, MaskOf(AuthorityComponent::IntentGeneration));
  }
  if (vector.intent.empty()) {
    mask = Combine(mask, MaskOf(AuthorityComponent::IntentDigest));
  }
  if (vector.observation.empty()) {
    mask = Combine(mask, MaskOf(AuthorityComponent::ObservationIdentity));
  }
  if (vector.observation_generation.value() == 0) {
    mask = Combine(mask, MaskOf(AuthorityComponent::ObservationGeneration));
  }
  if (vector.observation_reporter.empty()) {
    mask = Combine(mask, MaskOf(AuthorityComponent::ObservationReporter));
  }
  if (vector.observation_epoch != live.coordinator_epoch) {
    mask = Combine(mask, MaskOf(AuthorityComponent::ObservationEpoch));
  }
  if (!freshness_ok) {
    mask = Combine(mask, MaskOf(AuthorityComponent::ObservationFreshness));
  }
  if (fence_holds) {
    mask = Combine(mask, MaskOf(AuthorityComponent::Fence));
  }
  assessment.failed_components = mask;
  assessment.proven = (mask == 0);
  if (assessment.proven) {
    assessment.reason = ReasonCode::AuthorityProven;
    return assessment;
  }
  if ((mask & MaskOf(AuthorityComponent::Fence)) != 0) {
    assessment.reason = ReasonCode::ScopeFenced;
    return assessment;
  }
  if ((mask & MaskOf(AuthorityComponent::ObservationFreshness)) != 0) {
    assessment.reason = freshness_reason;
    return assessment;
  }
  if ((mask & MaskOf(AuthorityComponent::CoordinatorEpoch)) != 0 ||
      (mask & MaskOf(AuthorityComponent::BootIdentity)) != 0) {
    assessment.reason = ReasonCode::AuthorityEpochMismatch;
    return assessment;
  }
  if ((mask & (MaskOf(AuthorityComponent::PolicyId) | MaskOf(AuthorityComponent::PolicyVersion) |
               MaskOf(AuthorityComponent::PolicyDigest))) != 0) {
    assessment.reason = ReasonCode::AuthorityPolicyMismatch;
    return assessment;
  }
  if ((mask & (MaskOf(AuthorityComponent::IntentGeneration) |
               MaskOf(AuthorityComponent::IntentDigest))) != 0) {
    assessment.reason = ReasonCode::AuthorityIntentGenerationMismatch;
    return assessment;
  }
  if ((mask & MaskOf(AuthorityComponent::Definition)) != 0) {
    assessment.reason = ReasonCode::AuthorityDefinitionMismatch;
    return assessment;
  }
  assessment.reason = ReasonCode::AuthorityObservationGenerationMismatch;
  return assessment;
}

}  // namespace fabric_reconciliation
}  // namespace summon
