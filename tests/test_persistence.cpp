// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence adversarial proofs. Every durable format is attacked directly:
// truncated prefixes, corrupted digests, impossible lengths, invalid
// enumerations, sequence regression, trailing garbage and torn tails.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/journal.hpp"
#include "summon/fabric_reconciliation/snapshot.hpp"

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string content;
  if (!stream) {
    return content;
  }
  stream.seekg(0, std::ios::end);
  content.resize(static_cast<std::size_t>(stream.tellg()));
  stream.seekg(0, std::ios::beg);
  stream.read(&content[0], static_cast<std::streamsize>(content.size()));
  return content;
}

void WriteWholeFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

fr::Status OpenJournal(const std::filesystem::path& path, std::vector<fr::JournalRecord>& records,
                       fr::JournalOpenReport& report, fr::Journal& journal, bool create = false) {
  fr::JournalOpenOptions options;
  options.create_if_missing = create;
  return fr::Journal::Open(path, options, records, report, journal);
}

/// Builds a journal file with three records, then returns its bytes.
std::string BuildSeededJournal(const std::filesystem::path& path) {
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status created = OpenJournal(path, records, report, journal, true);
  if (!created.ok()) {
    throw std::runtime_error("seed failed: " + created.ToString());
  }
  for (int index = 0; index < 3; ++index) {
    const std::string payload = "payload-" + std::to_string(index);
    const fr::Status appended = journal.Append(fr::RecordType::StoreHeader, payload);
    if (!appended.ok()) {
      throw std::runtime_error("append failed: " + appended.ToString());
    }
  }
  journal.Close();
  return ReadWholeFile(path);
}

}  // namespace

FR_TEST(journal, header_is_integrity_checked) {
  ScratchDirectory scratch{"journal-header"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);

  fr::JournalHeader header;
  FR_CHECK(fr::ParseJournalHeaderImage(reinterpret_cast<const std::uint8_t*>(image.data()),
                                       fr::kJournalHeaderBytes, header));
  FR_CHECK(header.format_version == fr::kJournalFormatVersion);

  // Corrupt the digest.
  std::string bad_digest = image;
  bad_digest[80] = static_cast<char>(bad_digest[80] ^ 0x01);
  FR_CHECK(!fr::ParseJournalHeaderImage(reinterpret_cast<const std::uint8_t*>(bad_digest.data()),
                                        fr::kJournalHeaderBytes, header));

  // Corrupt a reserved byte.
  std::string bad_reserved = image;
  bad_reserved[45] = 1;
  FR_CHECK(!fr::ParseJournalHeaderImage(
      reinterpret_cast<const std::uint8_t*>(bad_reserved.data()), fr::kJournalHeaderBytes, header));

  // Unsupported version.
  std::string bad_version = image;
  bad_version[8] = 99;
  FR_CHECK(!fr::ParseJournalHeaderImage(
      reinterpret_cast<const std::uint8_t*>(bad_version.data()), fr::kJournalHeaderBytes, header));

  // Short buffer.
  FR_CHECK(!fr::ParseJournalHeaderImage(reinterpret_cast<const std::uint8_t*>(image.data()),
                                        fr::kJournalHeaderBytes - 1, header));
  FR_CHECK(!fr::ParseJournalHeaderImage(nullptr, fr::kJournalHeaderBytes, header));
}

FR_TEST(journal, refuses_a_file_shorter_than_its_header) {
  ScratchDirectory scratch{"journal-short"};
  const std::filesystem::path path = scratch.child("store.journal");
  WriteWholeFile(path, std::string(20, 'x'));
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::IntegrityFailure);
}

FR_TEST(journal, refuses_a_corrupt_header) {
  ScratchDirectory scratch{"journal-corrupt-header"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  image[3] = 'X';
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::JournalHeaderInvalid);
}

FR_TEST(journal, refuses_a_complete_record_with_a_broken_digest) {
  ScratchDirectory scratch{"journal-digest"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  // Flip a byte inside the first record payload.
  const std::size_t payload_byte = fr::kJournalHeaderBytes + fr::kRecordHeaderBytes + 2;
  image[payload_byte] = static_cast<char>(image[payload_byte] ^ 0x20);
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::JournalRecordDigestMismatch);
}

FR_TEST(journal, refuses_a_broken_digest_even_at_the_tail) {
  ScratchDirectory scratch{"journal-tail-digest"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  image[image.size() - 1] = static_cast<char>(image[image.size() - 1] ^ 0x40);
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::JournalRecordDigestMismatch);
}

FR_TEST(journal, refuses_an_impossible_declared_length) {
  ScratchDirectory scratch{"journal-length"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  const std::size_t length_offset = fr::kJournalHeaderBytes + 12;
  for (unsigned index = 0; index < 4; ++index) {
    image[length_offset + index] = static_cast<char>(0xff);
  }
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
}

FR_TEST(journal, refuses_an_unknown_record_type) {
  ScratchDirectory scratch{"journal-type"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  const std::size_t type_offset = fr::kJournalHeaderBytes + 6;
  image[type_offset] = static_cast<char>(0x7f);
  image[type_offset + 1] = 0;
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
}

FR_TEST(journal, refuses_trailing_garbage_that_has_a_plausible_header_region) {
  ScratchDirectory scratch{"journal-garbage"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  image.append(64, 'Z');
  WriteWholeFile(path, image);
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::JournalTrailingGarbage);
}

FR_TEST(journal, recovers_only_a_genuine_torn_tail) {
  ScratchDirectory scratch{"journal-torn"};
  const std::filesystem::path path = scratch.child("store.journal");
  const std::string complete = BuildSeededJournal(path);

  // Append a partial record: a valid magic and a declared payload that runs
  // past the end of the file.
  const std::string partial = fr::BuildRecordImage(fr::RecordSequence(4), fr::RecordType::StoreHeader,
                                                   std::string(512, 'p'));
  const std::string truncated = complete + partial.substr(0, fr::kRecordHeaderBytes + 100);
  WriteWholeFile(path, truncated);

  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK_STATUS_OK(status);
  FR_CHECK(report.recovered_torn_tail);
  FR_CHECK_EQ(report.recovered_bytes,
              static_cast<std::uint64_t>(fr::kRecordHeaderBytes + 100));
  FR_CHECK_EQ(records.size(), std::size_t(3));
  journal.Close();
  FR_CHECK_EQ(ReadWholeFile(path).size(), complete.size());
}

FR_TEST(journal, refuses_every_truncated_record_prefix) {
  ScratchDirectory scratch{"journal-prefix"};
  const std::filesystem::path path = scratch.child("store.journal");
  const std::string complete = BuildSeededJournal(path);
  const std::string extra = fr::BuildRecordImage(fr::RecordSequence(4), fr::RecordType::StoreHeader,
                                                 std::string(64, 'q'));
  for (std::size_t length = 1; length < fr::kRecordHeaderBytes; ++length) {
    WriteWholeFile(path, complete + extra.substr(0, length));
    std::vector<fr::JournalRecord> records;
    fr::JournalOpenReport report;
    fr::Journal journal;
    const fr::Status status = OpenJournal(path, records, report, journal);
    // A prefix shorter than the digest-protected header region is a torn tail
    // and is recovered; it is never treated as a record.
    FR_CHECK_STATUS_OK(status);
    FR_CHECK(report.recovered_torn_tail);
    FR_CHECK_EQ(records.size(), std::size_t(3));
    journal.Close();
  }
  for (std::size_t length = fr::kRecordHeaderBytes; length < extra.size(); ++length) {
    WriteWholeFile(path, complete + extra.substr(0, length));
    std::vector<fr::JournalRecord> records;
    fr::JournalOpenReport report;
    fr::Journal journal;
    const fr::Status status = OpenJournal(path, records, report, journal);
    FR_CHECK_STATUS_OK(status);
    FR_CHECK(report.recovered_torn_tail);
    FR_CHECK_EQ(records.size(), std::size_t(3));
    journal.Close();
  }
}

FR_TEST(journal, detects_a_sequence_regression_in_the_middle) {
  ScratchDirectory scratch{"journal-regression"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::string image = BuildSeededJournal(path);
  // Rewrite the sequence of the third record to duplicate the second, then
  // recompute that record's digest so the record itself is intact. A duplicate
  // sequence must be refused even when every byte of the record is well formed.
  const std::size_t record_bytes =
      fr::kRecordHeaderBytes + std::string("payload-0").size();
  const std::size_t second_record = fr::kJournalHeaderBytes + record_bytes;
  const std::size_t third_record = second_record + record_bytes;
  for (unsigned index = 0; index < 8; ++index) {
    image[third_record + 16 + index] = image[second_record + 16 + index];
  }
  const std::string_view record(image.data() + third_record, record_bytes);
  fr::Sha256 hasher;
  hasher.Update(reinterpret_cast<const std::uint8_t*>(record.data()), fr::kRecordDigestOffset);
  hasher.Update(record.data() + fr::kRecordHeaderBytes,
                std::string("payload-2").size());
  const fr::Sha256Digest digest = hasher.Final();
  std::memcpy(&image[third_record + fr::kRecordDigestOffset], digest.data(), digest.size());
  WriteWholeFile(path, image);

  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  const fr::Status status = OpenJournal(path, records, report, journal);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::JournalSequenceRegression);
}

FR_TEST(journal, refuses_oversized_payloads_before_allocating) {
  ScratchDirectory scratch{"journal-oversize"};
  const std::filesystem::path path = scratch.child("store.journal");
  std::vector<fr::JournalRecord> records;
  fr::JournalOpenReport report;
  fr::Journal journal;
  FR_CHECK_STATUS_OK(OpenJournal(path, records, report, journal, true));
  const fr::RuntimeLimits limits;
  const std::string too_large(limits.max_journal_payload_bytes + 1, 'x');
  const fr::Status status = journal.Append(fr::RecordType::StoreHeader, too_large);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::LimitExceeded);
  journal.Close();
}

FR_TEST(journal, record_codec_rejects_impossible_headers) {
  const std::string image =
      fr::BuildRecordImage(fr::RecordSequence(1), fr::RecordType::PolicyPut, "abc");
  FR_CHECK(fr::VerifyRecordImage(image));

  fr::RecordType type = fr::RecordType::Invalid;
  std::uint32_t payload_len = 0;
  fr::RecordSequence sequence;
  std::uint16_t flags = 0;
  FR_CHECK(fr::DecodeRecordHeader(reinterpret_cast<const std::uint8_t*>(image.data()),
                                  image.size(), 1024, type, payload_len, sequence, flags));
  FR_CHECK(type == fr::RecordType::PolicyPut);
  FR_CHECK_EQ(payload_len, 3u);
  FR_CHECK_EQ(sequence.value(), 1ull);

  FR_CHECK(!fr::DecodeRecordHeader(reinterpret_cast<const std::uint8_t*>(image.data()),
                                   fr::kRecordHeaderBytes - 1, 1024, type, payload_len, sequence,
                                   flags));
  FR_CHECK(!fr::DecodeRecordHeader(reinterpret_cast<const std::uint8_t*>(image.data()),
                                   image.size(), 2, type, payload_len, sequence, flags));
  FR_CHECK(!fr::VerifyRecordImage(image.substr(0, image.size() - 1)));
}

FR_TEST(snapshot, round_trips_and_detects_tampering) {
  ScratchDirectory scratch{"snapshot"};
  const std::filesystem::path path = scratch.child("store.snapshot");
  fr::SnapshotHeader header;
  header.format_version = fr::kSnapshotFormatVersion;
  header.header_bytes = static_cast<std::uint32_t>(fr::kSnapshotHeaderBytes);
  header.store = fr::StoreId::Generate();
  header.watermark = fr::RecordSequence(42);
  header.epoch = fr::CoordinatorEpoch(7);
  header.created_unix_ms = 1234;
  const std::string payload = "the durable payload";
  header.payload_len = payload.size();
  FR_CHECK_STATUS_OK(fr::WriteSnapshotAtomic(path, header, payload));

  fr::SnapshotHeader read_header;
  std::string read_payload;
  FR_CHECK_STATUS_OK(
      fr::ReadSnapshot(path, fr::RuntimeLimits{}, read_header, read_payload));
  FR_CHECK(read_header.store == header.store);
  FR_CHECK(read_header.watermark.value() == 42);
  FR_CHECK(read_header.epoch.value() == 7);
  FR_CHECK_EQ(read_payload, payload);

  std::string image = ReadWholeFile(path);
  image[image.size() - 1] = static_cast<char>(image[image.size() - 1] ^ 0x08);
  WriteWholeFile(path, image);
  FR_CHECK(!fr::ReadSnapshot(path, fr::RuntimeLimits{}, read_header, read_payload).ok());

  // Trailing bytes are refused.
  WriteWholeFile(path, image + "x");
  FR_CHECK(!fr::ReadSnapshot(path, fr::RuntimeLimits{}, read_header, read_payload).ok());
}

FR_TEST(snapshot, every_truncated_prefix_is_refused) {
  ScratchDirectory scratch{"snapshot-prefix"};
  const std::filesystem::path path = scratch.child("store.snapshot");
  fr::SnapshotHeader header;
  header.format_version = fr::kSnapshotFormatVersion;
  header.header_bytes = static_cast<std::uint32_t>(fr::kSnapshotHeaderBytes);
  header.store = fr::StoreId::Generate();
  header.watermark = fr::RecordSequence(1);
  header.epoch = fr::CoordinatorEpoch(1);
  header.payload_len = 8;
  FR_CHECK_STATUS_OK(fr::WriteSnapshotAtomic(path, header, "12345678"));
  const std::string complete = ReadWholeFile(path);
  fr::SnapshotHeader parsed;
  std::string_view payload;
  for (std::size_t length = 0; length < complete.size(); ++length) {
    FR_CHECK(!fr::ParseSnapshotImage(complete.substr(0, length), parsed, payload));
  }
  FR_CHECK(fr::ParseSnapshotImage(complete, parsed, payload));
}

FR_TEST(snapshot, a_stale_staging_file_is_removed_without_touching_the_snapshot) {
  ScratchDirectory scratch{"snapshot-staging"};
  const std::filesystem::path path = scratch.child("store.snapshot");
  fr::SnapshotHeader header;
  header.format_version = fr::kSnapshotFormatVersion;
  header.header_bytes = static_cast<std::uint32_t>(fr::kSnapshotHeaderBytes);
  header.store = fr::StoreId::Generate();
  header.watermark = fr::RecordSequence(3);
  header.epoch = fr::CoordinatorEpoch(2);
  header.payload_len = 4;
  FR_CHECK_STATUS_OK(fr::WriteSnapshotAtomic(path, header, "data"));

  const std::filesystem::path staging = std::filesystem::path(path.string() + ".staging");
  WriteWholeFile(staging, "partial");
  FR_CHECK(std::filesystem::exists(staging));
  FR_CHECK_STATUS_OK(fr::RemoveStagingFile(path));
  FR_CHECK(!std::filesystem::exists(staging));
  FR_CHECK(std::filesystem::exists(path));
  fr::SnapshotHeader read_header;
  std::string read_payload;
  FR_CHECK_STATUS_OK(fr::ReadSnapshot(path, fr::RuntimeLimits{}, read_header, read_payload));
  FR_CHECK_EQ(read_payload, std::string("data"));
}

FR_TEST(store, torn_tail_is_recovered_and_reported) {
  ScratchDirectory scratch{"store-torn"};
  const std::filesystem::path directory = scratch.path();
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(directory);
    const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 4, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
  }
  const std::filesystem::path journal = directory / "store.journal";
  const std::string complete = ReadWholeFile(journal);
  const std::string partial = fr::BuildRecordImage(fr::RecordSequence(999),
                                                   fr::RecordType::StoreHeader,
                                                   std::string(400, 'p'))
                                  .substr(0, fr::kRecordHeaderBytes + 7);
  WriteWholeFile(journal, complete + partial);

  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(directory);
  FR_CHECK(reopened->boot_report().recovered_torn_tail);
  FR_CHECK_EQ(reopened->boot_report().recovered_bytes,
              static_cast<std::uint64_t>(fr::kRecordHeaderBytes + 7));
  const auto intent = reopened->CurrentIntent(RequireScope("fabric/rack-7"));
  FR_CHECK_STATUS_OK(intent);
}

FR_TEST(store, a_corrupt_record_is_never_silently_truncated) {
  ScratchDirectory scratch{"store-corrupt"};
  const std::filesystem::path directory = scratch.path();
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(directory);
    const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 4, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult first;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, first));
    const fr::IntentDocument second = MakeIntent("fabric/rack-7", 2, 4, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(second, result));
  }
  const std::filesystem::path journal = directory / "store.journal";
  std::string image = ReadWholeFile(journal);
  const std::size_t second_record = image.size() / 2 + 40;
  image[second_record] = static_cast<char>(image[second_record] ^ 0x11);
  WriteWholeFile(journal, image);

  fr::EngineOptions options;
  options.store_dir = directory;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status status = fr::ReconciliationEngine::Open(options, engine);
  FR_CHECK(!status.ok());
  FR_CHECK(status.code() == fr::StatusCode::IntegrityFailure);
  // The file must be left exactly as found.
  FR_CHECK_EQ(ReadWholeFile(journal), image);
}

FR_TEST(store, compaction_preserves_every_durable_fact) {
  ScratchDirectory scratch{"store-compact"};
  const std::filesystem::path directory = scratch.path();
  std::vector<fr::AttemptRecord> attempts_before;
  std::vector<fr::ReconciliationOutcome> outcomes_before;
  fr::CoordinatorEpoch epoch_before;
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(directory);
    const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 6, fr::EvidenceClass::Synthetic);
    const fr::ObservationSubmission observation =
        MakeObservation("fabric/rack-7", "reporter/a", 1, 6, 2, false, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult commit_result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
    fr::ObservationCommitResult observation_result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));
    fr::PlanRequest request;
    request.scope = RequireScope("fabric/rack-7");
    request.now_unix_ms = 1000;
    const auto plan = engine->Plan(request);
    FR_CHECK_STATUS_OK(plan);
    FR_CHECK_STATUS_OK(engine->Dispatch(*plan));
    const auto attempts = engine->Attempts(RequireScope("fabric/rack-7"));
    FR_CHECK_STATUS_OK(attempts);
    attempts_before = *attempts;
    FR_CHECK_STATUS_OK(engine->Compact());
    const auto outcomes = engine->Outcomes(RequireScope("fabric/rack-7"));
    FR_CHECK_STATUS_OK(outcomes);
    outcomes_before = *outcomes;
    epoch_before = engine->epoch();
  }
  std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(directory);
  FR_CHECK(reopened->boot_report().snapshot_loaded);
  FR_CHECK(reopened->epoch().value() == epoch_before.value() + 1);
  const auto attempts = reopened->Attempts(RequireScope("fabric/rack-7"));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), attempts_before.size());
  const auto outcomes = reopened->Outcomes(RequireScope("fabric/rack-7"));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK(outcomes->size() >= outcomes_before.size());
  const auto intent = reopened->CurrentIntent(RequireScope("fabric/rack-7"));
  FR_CHECK_STATUS_OK(intent);
  FR_CHECK_EQ(intent->generation.value(), 1ull);
}

FR_TEST(store, retained_evidence_history_is_bounded) {
  ScratchDirectory scratch{"store-bounded"};
  fr::EngineOptions options;
  options.store_dir = scratch.path();
  options.limits.max_retained_observations_per_scope = 3;
  options.limits.max_retained_outcomes = 4;
  options.auto_compact_after_records = 0;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  FR_CHECK_STATUS_OK(fr::ReconciliationEngine::Open(options, engine));

  for (std::uint64_t generation = 1; generation <= 10; ++generation) {
    const fr::ObservationSubmission observation = MakeObservation(
        "fabric/rack-7", "reporter/a", generation, 2, 5, false, fr::EvidenceClass::Synthetic);
    fr::ObservationCommitResult result;
    FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, result));
  }
  const auto evidence = engine->ObservationEvidence(RequireScope("fabric/rack-7"));
  FR_CHECK_STATUS_OK(evidence);
  FR_CHECK_EQ(evidence->size(), std::size_t(3));
  FR_CHECK(evidence->back().generation.value() == 10);
}

FR_TEST(store, durable_state_is_rejected_when_the_store_identity_mismatches) {
  ScratchDirectory scratch{"store-identity"};
  const std::filesystem::path first = scratch.child("first");
  const std::filesystem::path second = scratch.child("second");
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(first);
    const fr::IntentDocument intent = MakeIntent("fabric/rack-7", 1, 2, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
    FR_CHECK_STATUS_OK(engine->Compact());
  }
  {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(second);
    const fr::IntentDocument intent = MakeIntent("fabric/rack-8", 1, 2, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
  }
  // Replace the first store's journal with the second store's journal.
  std::error_code error;
  std::filesystem::copy_file(second / "store.journal", first / "store.journal",
                             std::filesystem::copy_options::overwrite_existing, error);
  FR_CHECK(!error);
  fr::EngineOptions options;
  options.store_dir = first;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status status = fr::ReconciliationEngine::Open(options, engine);
  FR_CHECK(!status.ok());
  FR_CHECK(status.reason() == fr::ReasonCode::SnapshotStoreMismatch);
}
