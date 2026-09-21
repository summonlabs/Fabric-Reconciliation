// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/lineage.hpp"

namespace summon {
namespace fabric_reconciliation {

const char* ToText(AttemptState value) noexcept {
  switch (value) {
    case AttemptState::Invalid: return "INVALID";
    case AttemptState::Issued: return "ISSUED";
    case AttemptState::Dispatched: return "DISPATCHED";
    case AttemptState::Acknowledged: return "ACKNOWLEDGED";
    case AttemptState::Applied: return "APPLIED";
    case AttemptState::Verified: return "VERIFIED";
    case AttemptState::Failed: return "FAILED";
    case AttemptState::Abandoned: return "ABANDONED";
    case AttemptState::Superseded: return "SUPERSEDED";
    case AttemptState::Fenced: return "FENCED";
    case AttemptState::Interrupted: return "INTERRUPTED";
  }
  return "INVALID";
}

bool TryParseAttemptState(const char* text, AttemptState& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::string_view view(text);
  if (view == "ISSUED") { out = AttemptState::Issued; return true; }
  if (view == "DISPATCHED") { out = AttemptState::Dispatched; return true; }
  if (view == "ACKNOWLEDGED") { out = AttemptState::Acknowledged; return true; }
  if (view == "APPLIED") { out = AttemptState::Applied; return true; }
  if (view == "VERIFIED") { out = AttemptState::Verified; return true; }
  if (view == "FAILED") { out = AttemptState::Failed; return true; }
  if (view == "ABANDONED") { out = AttemptState::Abandoned; return true; }
  if (view == "SUPERSEDED") { out = AttemptState::Superseded; return true; }
  if (view == "FENCED") { out = AttemptState::Fenced; return true; }
  if (view == "INTERRUPTED") { out = AttemptState::Interrupted; return true; }
  return false;
}

bool IsTerminalAttemptState(AttemptState value) noexcept {
  switch (value) {
    case AttemptState::Verified:
    case AttemptState::Failed:
    case AttemptState::Abandoned:
    case AttemptState::Superseded:
    case AttemptState::Fenced:
    case AttemptState::Interrupted:
      return true;
    case AttemptState::Issued:
    case AttemptState::Dispatched:
    case AttemptState::Acknowledged:
    case AttemptState::Applied:
    case AttemptState::Invalid:
      return false;
  }
  return false;
}

bool IsInconclusiveTerminalState(AttemptState value) noexcept {
  switch (value) {
    case AttemptState::Failed:
    case AttemptState::Abandoned:
    case AttemptState::Superseded:
    case AttemptState::Fenced:
    case AttemptState::Interrupted:
      return true;
    default:
      return false;
  }
}

ReasonCode ReasonOf(AttemptState value) noexcept {
  switch (value) {
    case AttemptState::Issued: return ReasonCode::AttemptIssued;
    case AttemptState::Dispatched: return ReasonCode::AttemptIssued;
    case AttemptState::Acknowledged: return ReasonCode::AttemptAcknowledged;
    case AttemptState::Applied: return ReasonCode::AttemptApplied;
    case AttemptState::Verified: return ReasonCode::AttemptVerified;
    case AttemptState::Failed: return ReasonCode::AttemptFailed;
    case AttemptState::Abandoned: return ReasonCode::AttemptAbandoned;
    case AttemptState::Superseded: return ReasonCode::AttemptSuperseded;
    case AttemptState::Fenced: return ReasonCode::AttemptFencedByEpochAdvance;
    case AttemptState::Interrupted: return ReasonCode::AttemptInterruptedByRestart;
    case AttemptState::Invalid: return ReasonCode::AttemptUnknown;
  }
  return ReasonCode::AttemptUnknown;
}

std::string EncodeFence(const FenceEntry& fence) {
  CanonicalWriter writer;
  writer.PutBytes(fence.id.str());
  writer.PutBytes(fence.scope.str());
  writer.PutBytes(fence.subject.str());
  writer.PutU64(fence.fenced_below.value());
  writer.PutU16(static_cast<std::uint16_t>(fence.reason));
  writer.PutBytes(fence.detail);
  writer.PutI64(fence.created_unix_ms);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

bool DecodeFence(CanonicalReader& reader, FenceEntry& out) {
  FenceEntry fence;
  std::string id_text;
  std::string scope_text;
  std::string subject_text;
  std::string detail;
  std::uint64_t fenced_below = 0;
  std::uint16_t reason = 0;
  std::int64_t created = 0;
  if (!reader.ReadBytes(id_text) || !reader.ReadBytes(scope_text) ||
      !reader.ReadBytes(subject_text) || !reader.ReadU64(fenced_below) ||
      !reader.ReadU16(reason) || !reader.ReadBytes(detail) || !reader.ReadI64(created)) {
    return false;
  }
  const auto id = FenceId::TryParse(id_text);
  const auto scope = ScopeId::TryParse(scope_text);
  if (!id.has_value() || !scope.has_value()) {
    return false;
  }
  if (!subject_text.empty()) {
    const auto subject = SubjectId::TryParse(subject_text);
    if (!subject.has_value()) {
      return false;
    }
    fence.subject = *subject;
  }
  if (reason > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored)) {
    return false;
  }
  if (detail.size() > kMaxIdentityLength * 4) {
    return false;
  }
  fence.id = *id;
  fence.scope = *scope;
  fence.fenced_below = CoordinatorEpoch(fenced_below);
  fence.reason = static_cast<ReasonCode>(reason);
  fence.detail = std::move(detail);
  fence.created_unix_ms = created;
  out = std::move(fence);
  return true;
}

std::string EncodeOutcome(const ReconciliationOutcome& outcome) {
  CanonicalWriter writer;
  writer.PutDigest(outcome.idempotency_key);
  writer.PutBytes(outcome.scope.str());
  writer.PutBytes(outcome.subject.str());
  writer.PutU8(static_cast<std::uint8_t>(outcome.action));
  writer.PutU64(outcome.attempt.value());
  writer.PutU64(outcome.epoch.value());
  writer.PutBytes(std::string_view(reinterpret_cast<const char*>(outcome.boot.bytes().data()),
                                   outcome.boot.bytes().size()));
  writer.PutU8(static_cast<std::uint8_t>(outcome.terminal_state));
  writer.PutU16(static_cast<std::uint16_t>(outcome.reason));
  writer.PutI64(outcome.committed_unix_ms);
  writer.PutBool(outcome.verified);
  writer.PutU64(outcome.verified_generation.value());
  writer.PutBool(outcome.ambiguous);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

bool DecodeOutcome(CanonicalReader& reader, ReconciliationOutcome& out) {
  ReconciliationOutcome outcome;
  std::string scope_text;
  std::string subject_text;
  std::string boot_text;
  std::uint8_t action = 0;
  std::uint64_t attempt = 0;
  std::uint64_t epoch = 0;
  std::uint8_t terminal = 0;
  std::uint16_t reason = 0;
  std::int64_t committed = 0;
  bool verified = false;
  std::uint64_t verified_generation = 0;
  bool ambiguous = false;
  if (!reader.ReadDigest(outcome.idempotency_key) || !reader.ReadBytes(scope_text) ||
      !reader.ReadBytes(subject_text) || !reader.ReadU8(action) || !reader.ReadU64(attempt) ||
      !reader.ReadU64(epoch) || !reader.ReadBytes(boot_text) || !reader.ReadU8(terminal) ||
      !reader.ReadU16(reason) || !reader.ReadI64(committed) || !reader.ReadBool(verified) ||
      !reader.ReadU64(verified_generation) || !reader.ReadBool(ambiguous)) {
    return false;
  }
  if (boot_text.size() != 16) {
    return false;
  }
  if (action > static_cast<std::uint8_t>(ActionKind::WithdrawSubject)) {
    return false;
  }
  if (terminal > static_cast<std::uint8_t>(AttemptState::Interrupted)) {
    return false;
  }
  if (reason > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored)) {
    return false;
  }
  const auto scope = ScopeId::TryParse(scope_text);
  const auto subject = SubjectId::TryParse(subject_text);
  if (!scope.has_value() || !subject.has_value()) {
    return false;
  }
  std::array<std::uint8_t, 16> boot_bytes{};
  for (std::size_t index = 0; index < 16; ++index) {
    boot_bytes[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(boot_text[index]));
  }
  outcome.scope = *scope;
  outcome.subject = *subject;
  outcome.action = static_cast<ActionKind>(action);
  outcome.attempt = AttemptId(attempt);
  outcome.epoch = CoordinatorEpoch(epoch);
  outcome.boot = BootId::FromBytes(boot_bytes);
  outcome.terminal_state = static_cast<AttemptState>(terminal);
  outcome.reason = static_cast<ReasonCode>(reason);
  outcome.committed_unix_ms = committed;
  outcome.verified = verified;
  outcome.verified_generation = ObservationGeneration(verified_generation);
  outcome.ambiguous = ambiguous;
  out = std::move(outcome);
  return true;
}

}  // namespace fabric_reconciliation
}  // namespace summon
