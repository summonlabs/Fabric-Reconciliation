// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/store.hpp"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace summon {
namespace fabric_reconciliation {
namespace {

constexpr const char* kJournalFileName = "store.journal";
constexpr const char* kSnapshotFileName = "store.snapshot";
constexpr const char* kLockFileName = "store.lock";

/// Acquires an exclusive operating-system lock on the store directory.
///
/// Two live processes appending to one journal would interleave records and
/// break the contiguity the format guarantees, so ownership is enforced by the
/// operating system rather than by a convention. The lock is released by the
/// operating system when the process exits for any reason, including a hard
/// kill, so a crashed writer never leaves a lock that has to be cleaned up by
/// hand and there is no stale-lock heuristic to get wrong.
void* AcquireStoreLock(const std::filesystem::path& path) {
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  return handle;
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (descriptor < 0) {
    return nullptr;
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    return nullptr;
  }
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
#endif
}

void ReleaseStoreLock(void* handle) {
  if (handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle));
#else
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
  (void)::flock(descriptor, LOCK_UN);
  ::close(descriptor);
#endif
}

void EncodeAttempt(CanonicalWriter& writer, const AttemptRecord& attempt) noexcept {
  writer.PutU64(attempt.id.value());
  writer.PutDigest(attempt.idempotency_key);
  writer.PutBytes(attempt.scope.str());
  writer.PutBytes(attempt.subject.str());
  writer.PutU8(static_cast<std::uint8_t>(attempt.action));
  writer.PutU64(attempt.epoch.value());
  writer.PutBytes(std::string_view(reinterpret_cast<const char*>(attempt.boot.bytes().data()),
                                   attempt.boot.bytes().size()));
  attempt.authority.Encode(writer);
  writer.PutU8(static_cast<std::uint8_t>(attempt.state));
  writer.PutI64(attempt.issued_unix_ms);
  writer.PutU32(static_cast<std::uint32_t>(attempt.transitions.size() & 0xffffffffu));
  for (const AttemptTransition& transition : attempt.transitions) {
    writer.PutU8(static_cast<std::uint8_t>(transition.from));
    writer.PutU8(static_cast<std::uint8_t>(transition.to));
    writer.PutU64(transition.epoch.value());
    writer.PutBytes(std::string_view(reinterpret_cast<const char*>(transition.boot.bytes().data()),
                                     transition.boot.bytes().size()));
    writer.PutI64(transition.at_unix_ms);
    writer.PutU16(static_cast<std::uint16_t>(transition.reason));
    writer.PutU16(static_cast<std::uint16_t>(transition.verification_reason));
  }
}

bool DecodeAttempt(CanonicalReader& reader, AttemptRecord& out, const RuntimeLimits& limits) {
  AttemptRecord attempt;
  std::string scope_text;
  std::string subject_text;
  std::string boot_text;
  std::uint64_t id = 0;
  std::uint8_t action = 0;
  std::uint64_t epoch = 0;
  std::uint8_t state = 0;
  std::int64_t issued = 0;
  std::uint32_t transitions = 0;
  if (!reader.ReadU64(id) || !reader.ReadDigest(attempt.idempotency_key) ||
      !reader.ReadBytes(scope_text) || !reader.ReadBytes(subject_text) ||
      !reader.ReadU8(action) || !reader.ReadU64(epoch) || !reader.ReadBytes(boot_text) ||
      boot_text.size() != 16 || !AuthorityVector::Decode(reader, attempt.authority) ||
      !reader.ReadU8(state) || !reader.ReadI64(issued) || !reader.ReadU32(transitions)) {
    return false;
  }
  if (id == 0 || action > static_cast<std::uint8_t>(ActionKind::WithdrawSubject) ||
      state == 0 || state > static_cast<std::uint8_t>(AttemptState::Interrupted) ||
      static_cast<std::size_t>(transitions) > limits.max_attempt_transitions) {
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
  attempt.id = AttemptId(id);
  attempt.scope = *scope;
  attempt.subject = *subject;
  attempt.action = static_cast<ActionKind>(action);
  attempt.epoch = CoordinatorEpoch(epoch);
  attempt.boot = BootId::FromBytes(boot_bytes);
  attempt.state = static_cast<AttemptState>(state);
  attempt.issued_unix_ms = issued;
  for (std::uint32_t index = 0; index < transitions; ++index) {
    AttemptTransition transition;
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint64_t transition_epoch = 0;
    std::string transition_boot;
    std::uint16_t reason = 0;
    std::uint16_t verification = 0;
    if (!reader.ReadU8(from) || !reader.ReadU8(to) || !reader.ReadU64(transition_epoch) ||
        !reader.ReadBytes(transition_boot) || transition_boot.size() != 16 ||
        !reader.ReadI64(transition.at_unix_ms) || !reader.ReadU16(reason) ||
        !reader.ReadU16(verification)) {
      return false;
    }
    if (from > static_cast<std::uint8_t>(AttemptState::Interrupted) ||
        to > static_cast<std::uint8_t>(AttemptState::Interrupted) ||
        reason > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored) ||
        verification > static_cast<std::uint16_t>(ReasonCode::RestartLeasesNotRestored)) {
      return false;
    }
    std::array<std::uint8_t, 16> transition_boot_bytes{};
    for (std::size_t byte = 0; byte < 16; ++byte) {
      transition_boot_bytes[byte] =
          static_cast<std::uint8_t>(static_cast<unsigned char>(transition_boot[byte]));
    }
    transition.from = static_cast<AttemptState>(from);
    transition.to = static_cast<AttemptState>(to);
    transition.epoch = CoordinatorEpoch(transition_epoch);
    transition.boot = BootId::FromBytes(transition_boot_bytes);
    transition.reason = static_cast<ReasonCode>(reason);
    transition.verification_reason = static_cast<ReasonCode>(verification);
    attempt.transitions.push_back(std::move(transition));
  }
  out = std::move(attempt);
  return true;
}

std::string EncodeAttemptRecord(const AttemptRecord& attempt) {
  CanonicalWriter writer;
  EncodeAttempt(writer, attempt);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

}  // namespace

DurableStore::DurableStore(DurableStore&& other) noexcept
    : directory_(std::move(other.directory_)),
      journal_path_(std::move(other.journal_path_)),
      snapshot_path_(std::move(other.snapshot_path_)),
      staging_path_(std::move(other.staging_path_)),
      lock_path_(std::move(other.lock_path_)),
      lock_handle_(other.lock_handle_),
      limits_(other.limits_),
      state_(std::move(other.state_)),
      journal_(std::move(other.journal_)),
      read_only_(other.read_only_) {
  other.lock_handle_ = nullptr;
}

DurableStore& DurableStore::operator=(DurableStore&& other) noexcept {
  if (this != &other) {
    Close();
    directory_ = std::move(other.directory_);
    journal_path_ = std::move(other.journal_path_);
    snapshot_path_ = std::move(other.snapshot_path_);
    staging_path_ = std::move(other.staging_path_);
    lock_path_ = std::move(other.lock_path_);
    lock_handle_ = other.lock_handle_;
    limits_ = other.limits_;
    state_ = std::move(other.state_);
    journal_ = std::move(other.journal_);
    read_only_ = other.read_only_;
    other.lock_handle_ = nullptr;
  }
  return *this;
}

DurableStore::~DurableStore() { Close(); }

void DurableStore::Close() noexcept {
  journal_.Close();
  ReleaseStoreLock(lock_handle_);
  lock_handle_ = nullptr;
}

Status DurableStore::Open(const std::filesystem::path& directory, const RuntimeLimits& limits,
                          bool create_if_missing, bool read_only, DurableStore& out,
                          StoreOpenReport& report) {
  DurableStore store;
  store.read_only_ = read_only;
  store.directory_ = directory;
  store.journal_path_ = directory / kJournalFileName;
  store.snapshot_path_ = directory / kSnapshotFileName;
  store.staging_path_ = std::filesystem::path(store.snapshot_path_.string() + ".staging");
  store.lock_path_ = directory / kLockFileName;
  store.limits_ = limits;
  report = StoreOpenReport{};

  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    if (!create_if_missing) {
      return Status(StatusCode::NotFound, ReasonCode::DurableWriteFailed,
                    "store directory does not exist");
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
      return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                    "cannot create store directory");
    }
    report.created = true;
  } else if (!std::filesystem::is_directory(directory, error)) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "store path exists and is not a directory");
  }

  // Exclusive ownership. A second live process is refused outright rather than
  // allowed to interleave appends into one journal.
  store.lock_handle_ = AcquireStoreLock(store.lock_path_);
  if (store.lock_handle_ == nullptr) {
    return Status(StatusCode::ConflictState, ReasonCode::DurableWriteFailed,
                  "store is already open by another live process");
  }

  // A staging file left behind means the previous publication did not reach its
  // rename. It is removed; the published snapshot is untouched. A read-only
  // open never writes, so it leaves staging files exactly as found.
  if (!read_only) {
    const Status staging = RemoveStagingFile(store.snapshot_path_);
    if (!staging.ok()) {
      return staging;
    }
  }

  bool have_snapshot = false;
  SnapshotHeader snapshot_header;
  std::string snapshot_payload;
  const Status snapshot_status =
      ReadSnapshot(store.snapshot_path_, limits, snapshot_header, snapshot_payload);
  if (snapshot_status.ok()) {
    have_snapshot = true;
    report.snapshot_loaded = true;
  } else if (snapshot_status.code() != StatusCode::NotFound) {
    return snapshot_status;
  }

  std::vector<JournalRecord> records;
  JournalOpenReport journal_report;
  const JournalOpenOptions journal_options{limits, create_if_missing && !read_only, read_only};
  const Status journal_status =
      Journal::Open(store.journal_path_, journal_options, records, journal_report, store.journal_);
  if (!journal_status.ok()) {
    return journal_status;
  }
  report.created = report.created || journal_report.created;
  report.recovered_torn_tail = journal_report.recovered_torn_tail;
  report.recovered_bytes = journal_report.recovered_bytes;
  for (const ReasonCode note : journal_report.notes) {
    report.notes.push_back(note);
  }
  report.last_sequence = journal_report.last_sequence;

  if (have_snapshot) {
    if (!(snapshot_header.store == journal_report.header.store)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotStoreMismatch,
                    "snapshot and journal belong to different stores");
    }
    if (snapshot_header.watermark > journal_report.last_sequence) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotAheadOfJournal,
                    "snapshot watermark is ahead of the journal");
    }
    store.state_.store = snapshot_header.store;
    store.state_.last_epoch = snapshot_header.epoch;
    const Status applied = store.ApplySnapshotPayload(snapshot_payload);
    if (!applied.ok()) {
      return applied;
    }
  } else {
    store.state_.store = journal_report.header.store;
  }

  const RecordSequence watermark =
      have_snapshot ? snapshot_header.watermark : RecordSequence(0);
  for (const JournalRecord& record : records) {
    if (record.sequence <= watermark) {
      ++report.records_skipped_by_watermark;
      continue;
    }
    const Status applied = store.ApplyRecord(record);
    if (!applied.ok()) {
      return applied;
    }
    ++report.records_replayed;
  }

  store.state_.sequence = journal_report.last_sequence;
  report.last_epoch = store.state_.last_epoch;
  report.attempts_restored = store.state_.attempts.size();
  out = std::move(store);
  return Status::Ok();
}

Status DurableStore::ApplyRecord(const JournalRecord& record) {
  return ApplyPayload(record.type, record.payload);
}

Status DurableStore::ApplyPayload(RecordType type, std::string_view payload) {
  switch (type) {
    case RecordType::StoreHeader:
      return Status::Ok();
    case RecordType::PolicyPut: {
      ReconciliationPolicy policy;
      if (!DecodePolicyExact(payload, policy, limits_)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "policy record failed validation");
      }
      state_.policy = std::move(policy);
      state_.has_policy = true;
      return Status::Ok();
    }
    case RecordType::IntentCommit: {
      IntentDocument document;
      if (!DecodeIntentExact(payload, document, limits_)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "intent record failed validation");
      }
      auto& generations = state_.intent_generations[document.scope];
      generations.push_back(document.generation);
      const std::size_t bound = limits_.max_retained_observations_per_scope;
      if (generations.size() > bound) {
        generations.erase(generations.begin(),
                          generations.begin() + static_cast<std::ptrdiff_t>(generations.size() - bound));
      }
      state_.intents[document.scope] = std::move(document);
      return Status::Ok();
    }
    case RecordType::ObservationCommit: {
      ObservationDocument document;
      if (!DecodeObservationExact(payload, document, limits_)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "observation record failed validation");
      }
      const ScopeId scope = document.scope;
      auto& stream = state_.observations[scope];
      stream.push_back(std::move(document));
      TrimObservations(scope);
      return Status::Ok();
    }
    case RecordType::AttemptIssued:
    case RecordType::AttemptTransition: {
      CanonicalReader reader(payload);
      AttemptRecord attempt;
      if (!DecodeAttempt(reader, attempt, limits_) || !reader.AtEnd()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "attempt record failed validation");
      }
      if (attempt.id.value() >= state_.next_attempt.value()) {
        state_.next_attempt = AttemptId(attempt.id.value() + 1);
      }
      if (state_.next_attempt.value() == 0) {
        state_.next_attempt = AttemptId(1);
      }
      state_.attempts[attempt.id.value()] = std::move(attempt);
      TrimAttempts();
      return Status::Ok();
    }
    case RecordType::OutcomeCommit: {
      CanonicalReader reader(payload);
      ReconciliationOutcome outcome;
      if (!DecodeOutcome(reader, outcome) || !reader.AtEnd()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "outcome record failed validation");
      }
      state_.outcomes.push_back(std::move(outcome));
      TrimOutcomes();
      return Status::Ok();
    }
    case RecordType::FencePut: {
      CanonicalReader reader(payload);
      FenceEntry fence;
      if (!DecodeFence(reader, fence) || !reader.AtEnd()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "fence record failed validation");
      }
      state_.fences[fence.id] = std::move(fence);
      return Status::Ok();
    }
    case RecordType::FenceClear: {
      CanonicalReader reader(payload);
      std::string id_text;
      if (!reader.ReadBytes(id_text) || !reader.AtEnd()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "fence clear record failed validation");
      }
      const auto id = FenceId::TryParse(id_text);
      if (!id.has_value()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "fence clear record carries an invalid identity");
      }
      state_.fences.erase(*id);
      return Status::Ok();
    }
    case RecordType::EpochAdvance: {
      CanonicalReader reader(payload);
      std::uint64_t epoch = 0;
      if (!reader.ReadU64(epoch) || !reader.AtEnd()) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalEnumInvalid,
                      "epoch record failed validation");
      }
      if (epoch > state_.last_epoch.value()) {
        state_.last_epoch = CoordinatorEpoch(epoch);
      }
      return Status::Ok();
    }
    case RecordType::LineageTrim:
      return Status::Ok();
    case RecordType::Invalid:
    default:
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalRecordTypeUnknown,
                    "unknown record type");
  }
}

void DurableStore::TrimObservations(const ScopeId& scope) {
  auto found = state_.observations.find(scope);
  if (found == state_.observations.end()) {
    return;
  }
  auto& stream = found->second;
  while (stream.size() > limits_.max_retained_observations_per_scope) {
    stream.pop_front();
  }
}

void DurableStore::TrimAttempts() {
  while (state_.attempts.size() > limits_.max_retained_attempts) {
    auto victim = state_.attempts.end();
    for (auto iterator = state_.attempts.begin(); iterator != state_.attempts.end(); ++iterator) {
      if (IsTerminalAttemptState(iterator->second.state)) {
        victim = iterator;
        break;
      }
    }
    if (victim == state_.attempts.end()) {
      break;
    }
    state_.attempts.erase(victim);
  }
}

void DurableStore::TrimOutcomes() {
  while (state_.outcomes.size() > limits_.max_retained_outcomes) {
    state_.outcomes.pop_front();
  }
}

Status DurableStore::AppendAndApply(RecordType type, std::string_view payload) {
  const Status appended = journal_.Append(type, payload);
  if (!appended.ok()) {
    return appended;
  }
  state_.sequence = journal_.last_sequence();
  const Status applied = ApplyPayload(type, payload);
  if (!applied.ok()) {
    return applied;
  }
  return Status::Ok();
}

Status DurableStore::PutPolicy(const ReconciliationPolicy& policy) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodePolicy(policy);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "policy could not be encoded");
  }
  return AppendAndApply(RecordType::PolicyPut, payload);
}

Status DurableStore::CommitIntent(const IntentDocument& document) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodeIntent(document);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "intent could not be encoded");
  }
  return AppendAndApply(RecordType::IntentCommit, payload);
}

Status DurableStore::CommitObservation(const ObservationDocument& document) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodeObservation(document);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "observation could not be encoded");
  }
  return AppendAndApply(RecordType::ObservationCommit, payload);
}

Status DurableStore::IssueAttempt(const AttemptRecord& attempt) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  if (attempt.id.value() == 0) {
    return Status(StatusCode::Rejected, ReasonCode::UnsupportedInput,
                  "attempt identity zero is never valid");
  }
  const std::string payload = EncodeAttemptRecord(attempt);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "attempt could not be encoded");
  }
  return AppendAndApply(RecordType::AttemptIssued, payload);
}

Status DurableStore::TransitionAttempt(const AttemptRecord& attempt) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodeAttemptRecord(attempt);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "attempt could not be encoded");
  }
  return AppendAndApply(RecordType::AttemptTransition, payload);
}

Status DurableStore::CommitOutcome(const ReconciliationOutcome& outcome) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodeOutcome(outcome);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "outcome could not be encoded");
  }
  return AppendAndApply(RecordType::OutcomeCommit, payload);
}

Status DurableStore::PutFence(const FenceEntry& fence) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  const std::string payload = EncodeFence(fence);
  if (payload.empty()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "fence could not be encoded");
  }
  return AppendAndApply(RecordType::FencePut, payload);
}

Status DurableStore::ClearFence(const FenceId& fence) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  CanonicalWriter writer;
  writer.PutBytes(fence.str());
  if (!writer.ok()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "fence identity could not be encoded");
  }
  return AppendAndApply(RecordType::FenceClear, writer.buffer());
}

Status DurableStore::AdvanceEpoch(CoordinatorEpoch epoch) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  if (epoch <= state_.last_epoch) {
    return Status(StatusCode::ConflictState, ReasonCode::FenceEpochNotNewer,
                  "epoch advance must be strictly increasing");
  }
  CanonicalWriter writer;
  writer.PutU64(epoch.value());
  if (!writer.ok()) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "epoch could not be encoded");
  }
  return AppendAndApply(RecordType::EpochAdvance, writer.buffer());
}

std::string DurableStore::EncodeSnapshotPayload() const {
  CanonicalWriter writer;
  writer.PutU16(kCanonicalSchemaVersion);
  writer.PutU64(state_.last_epoch.value());
  writer.PutU64(state_.next_attempt.value());
  writer.PutBool(state_.has_policy);
  if (state_.has_policy) {
    writer.PutBytes(EncodePolicy(state_.policy));
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.intent_generations.size() & 0xffffffffu));
  for (const auto& entry : state_.intent_generations) {
    writer.PutBytes(entry.first.str());
    writer.PutU32(static_cast<std::uint32_t>(entry.second.size() & 0xffffffffu));
    for (const IntentGeneration generation : entry.second) {
      writer.PutU64(generation.value());
    }
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.intents.size() & 0xffffffffu));
  for (const auto& entry : state_.intents) {
    writer.PutBytes(EncodeIntent(entry.second));
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.observations.size() & 0xffffffffu));
  for (const auto& entry : state_.observations) {
    writer.PutBytes(entry.first.str());
    writer.PutU32(static_cast<std::uint32_t>(entry.second.size() & 0xffffffffu));
    for (const ObservationDocument& document : entry.second) {
      writer.PutBytes(EncodeObservation(document));
    }
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.attempts.size() & 0xffffffffu));
  for (const auto& entry : state_.attempts) {
    writer.PutBytes(EncodeAttemptRecord(entry.second));
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.outcomes.size() & 0xffffffffu));
  for (const ReconciliationOutcome& outcome : state_.outcomes) {
    writer.PutBytes(EncodeOutcome(outcome));
  }

  writer.PutU32(static_cast<std::uint32_t>(state_.fences.size() & 0xffffffffu));
  for (const auto& entry : state_.fences) {
    writer.PutBytes(EncodeFence(entry.second));
  }

  writer.PutU64(state_.compactions);
  if (!writer.ok()) {
    return std::string();
  }
  return writer.buffer();
}

Status DurableStore::ApplySnapshotPayload(std::string_view payload) {
  CanonicalReader reader(payload);
  std::uint16_t schema = 0;
  std::uint64_t last_epoch = 0;
  std::uint64_t next_attempt = 0;
  bool has_policy = false;
  if (!reader.ReadU16(schema) || schema != kCanonicalSchemaVersion ||
      !reader.ReadU64(last_epoch) || !reader.ReadU64(next_attempt) ||
      !reader.ReadBool(has_policy)) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot payload header is invalid");
  }
  state_.last_epoch = CoordinatorEpoch(last_epoch);
  state_.next_attempt = AttemptId(next_attempt == 0 ? 1 : next_attempt);
  if (has_policy) {
    std::string policy_bytes;
    if (!reader.ReadBytes(policy_bytes)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot policy is truncated");
    }
    ReconciliationPolicy policy;
    if (!DecodePolicyExact(policy_bytes, policy, limits_)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot policy failed validation");
    }
    state_.policy = std::move(policy);
    state_.has_policy = true;
  }

  std::uint32_t generation_scopes = 0;
  if (!reader.ReadU32(generation_scopes) ||
      static_cast<std::size_t>(generation_scopes) > limits_.max_scopes) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot generation table is invalid");
  }
  for (std::uint32_t index = 0; index < generation_scopes; ++index) {
    std::string scope_text;
    std::uint32_t count = 0;
    if (!reader.ReadBytes(scope_text) || !reader.ReadU32(count) ||
        static_cast<std::size_t>(count) > limits_.max_retained_observations_per_scope) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot generation entry is invalid");
    }
    const auto scope = ScopeId::TryParse(scope_text);
    if (!scope.has_value()) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot carries an invalid scope identity");
    }
    std::vector<IntentGeneration> generations;
    for (std::uint32_t entry = 0; entry < count; ++entry) {
      std::uint64_t value = 0;
      if (!reader.ReadU64(value)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                      "snapshot generation list is truncated");
      }
      generations.push_back(IntentGeneration(value));
    }
    state_.intent_generations[*scope] = std::move(generations);
  }

  std::uint32_t intent_count = 0;
  if (!reader.ReadU32(intent_count) ||
      static_cast<std::size_t>(intent_count) > limits_.max_scopes) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot intent table is invalid");
  }
  for (std::uint32_t index = 0; index < intent_count; ++index) {
    std::string intent_bytes;
    if (!reader.ReadBytes(intent_bytes)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot intent is truncated");
    }
    IntentDocument document;
    if (!DecodeIntentExact(intent_bytes, document, limits_)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot intent failed validation");
    }
    state_.intents[document.scope] = std::move(document);
  }

  std::uint32_t observation_scopes = 0;
  if (!reader.ReadU32(observation_scopes) ||
      static_cast<std::size_t>(observation_scopes) > limits_.max_scopes) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot observation table is invalid");
  }
  for (std::uint32_t index = 0; index < observation_scopes; ++index) {
    std::string scope_text;
    std::uint32_t count = 0;
    if (!reader.ReadBytes(scope_text) || !reader.ReadU32(count) ||
        static_cast<std::size_t>(count) > limits_.max_retained_observations_per_scope) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot observation entry is invalid");
    }
    const auto scope = ScopeId::TryParse(scope_text);
    if (!scope.has_value()) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot carries an invalid scope identity");
    }
    std::deque<ObservationDocument> stream;
    for (std::uint32_t entry = 0; entry < count; ++entry) {
      std::string observation_bytes;
      if (!reader.ReadBytes(observation_bytes)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                      "snapshot observation list is truncated");
      }
      ObservationDocument document;
      if (!DecodeObservationExact(observation_bytes, document, limits_)) {
        return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                      "snapshot observation failed validation");
      }
      stream.push_back(std::move(document));
    }
    state_.observations[*scope] = std::move(stream);
  }

  std::uint32_t attempt_count = 0;
  if (!reader.ReadU32(attempt_count) ||
      static_cast<std::size_t>(attempt_count) > limits_.max_retained_attempts) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot attempt table is invalid");
  }
  for (std::uint32_t index = 0; index < attempt_count; ++index) {
    std::string attempt_bytes;
    if (!reader.ReadBytes(attempt_bytes)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot attempt is truncated");
    }
    CanonicalReader attempt_reader(attempt_bytes);
    AttemptRecord attempt;
    if (!DecodeAttempt(attempt_reader, attempt, limits_) || !attempt_reader.AtEnd()) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot attempt failed validation");
    }
    state_.attempts[attempt.id.value()] = std::move(attempt);
  }

  std::uint32_t outcome_count = 0;
  if (!reader.ReadU32(outcome_count) ||
      static_cast<std::size_t>(outcome_count) > limits_.max_retained_outcomes) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot outcome table is invalid");
  }
  for (std::uint32_t index = 0; index < outcome_count; ++index) {
    std::string outcome_bytes;
    if (!reader.ReadBytes(outcome_bytes)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot outcome is truncated");
    }
    CanonicalReader outcome_reader(outcome_bytes);
    ReconciliationOutcome outcome;
    if (!DecodeOutcome(outcome_reader, outcome) || !outcome_reader.AtEnd()) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot outcome failed validation");
    }
    state_.outcomes.push_back(std::move(outcome));
  }

  std::uint32_t fence_count = 0;
  if (!reader.ReadU32(fence_count) ||
      static_cast<std::size_t>(fence_count) > limits_.max_scopes * 16u) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot fence table is invalid");
  }
  for (std::uint32_t index = 0; index < fence_count; ++index) {
    std::string fence_bytes;
    if (!reader.ReadBytes(fence_bytes)) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot fence is truncated");
    }
    CanonicalReader fence_reader(fence_bytes);
    FenceEntry fence;
    if (!DecodeFence(fence_reader, fence) || !fence_reader.AtEnd()) {
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "snapshot fence failed validation");
    }
    state_.fences[fence.id] = std::move(fence);
  }

  std::uint64_t compactions = 0;
  if (!reader.ReadU64(compactions) || !reader.AtEnd()) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot payload has trailing bytes or is truncated");
  }
  state_.compactions = compactions;
  return Status::Ok();
}

Status DurableStore::Compact() {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  ++state_.compactions;
  const std::string payload = EncodeSnapshotPayload();
  if (payload.empty()) {
    --state_.compactions;
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "snapshot payload could not be encoded");
  }
  SnapshotHeader header;
  header.format_version = kSnapshotFormatVersion;
  header.header_bytes = static_cast<std::uint32_t>(kSnapshotHeaderBytes);
  header.store = state_.store;
  header.watermark = state_.sequence;
  header.epoch = state_.last_epoch;
  header.created_unix_ms = WallClockMillis();
  header.payload_len = payload.size();

  const Status written = WriteSnapshotAtomic(snapshot_path_, header, payload);
  if (!written.ok()) {
    --state_.compactions;
    return written;
  }
  std::vector<JournalRecord> records;
  JournalOpenReport report;
  const Status reset = journal_.Reset(state_.store, records, report);
  if (!reset.ok()) {
    --state_.compactions;
    return reset;
  }
  state_.sequence = journal_.last_sequence();
  return Status::Ok();
}

Status DurableStore::SyncJournal() { return journal_.Sync(); }

Status DurableStore::ReopenJournalForTesting() {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "store is read-only");
  }
  std::vector<JournalRecord> records;
  JournalOpenReport report;
  return journal_.Reset(state_.store, records, report);
}

}  // namespace fabric_reconciliation
}  // namespace summon
