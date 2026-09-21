// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Versioned, integrity-checked, append-only durable journal.
//
// Format guarantees, all of them exercised adversarially in the test suite:
//   * every file starts with a fixed-size header carrying magic, format
//     version, header size, store identity and a digest over the header;
//   * every record is length-delimited and carries its own digest;
//   * record sequence numbers are contiguous, so a regression or a gap is a
//     hard integrity failure rather than a silent reordering;
//   * a file whose tail is a genuine torn write is recovered by discarding
//     only the incomplete suffix, and the recovery is reported;
//   * a complete record that fails its digest is never repaired and never
//     truncated: it is reported as an integrity failure.
//
// Write ordering: append, flush, fsync. Success is only reported after the
// durability barrier completes.

#ifndef SUMMON_FABRIC_RECONCILIATION_JOURNAL_HPP
#define SUMMON_FABRIC_RECONCILIATION_JOURNAL_HPP

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/version.hpp"

namespace summon {
namespace fabric_reconciliation {

inline constexpr std::size_t kJournalHeaderBytes = 96;
inline constexpr std::size_t kRecordHeaderBytes = 56;
inline constexpr std::size_t kRecordDigestOffset = 24;
inline constexpr std::uint32_t kRecordMagic = 0x43455246u;   // 'F','R','E','C'
inline constexpr std::uint16_t kRecordFormatVersion = 1;
inline constexpr std::uint16_t kJournalFlagsNone = 0;

/// Record vocabulary. An unknown type is refused, never skipped.
enum class RecordType : std::uint16_t {
  Invalid = 0,
  StoreHeader = 1,
  PolicyPut = 2,
  IntentCommit = 3,
  ObservationCommit = 4,
  AttemptIssued = 5,
  AttemptTransition = 6,
  OutcomeCommit = 7,
  FencePut = 8,
  FenceClear = 9,
  EpochAdvance = 10,
  LineageTrim = 11,
};

[[nodiscard]] FR_API const char* ToText(RecordType value) noexcept;
[[nodiscard]] FR_API bool TryParseRecordType(std::uint16_t raw, RecordType& out) noexcept;

struct FR_API JournalRecord {
  RecordSequence sequence;
  RecordType type{RecordType::Invalid};
  std::string payload;
};

/// Fixed header of a journal file.
///
/// The header carries the sequence the file continues from. A journal that was
/// replaced by a compaction keeps counting upwards, so record sequence numbers
/// are globally monotonic across every journal generation of one store and a
/// snapshot watermark always remains comparable with the live journal.
struct FR_API JournalHeader {
  std::uint32_t format_version{0};
  std::uint32_t header_bytes{0};
  StoreId store;
  UnixMillis created_unix_ms{0};
  /// Highest sequence already consumed before this file was created.
  RecordSequence base_sequence;
};

/// What happened while opening a journal. Reported to the caller; a torn tail
/// is never recovered silently.
struct FR_API JournalOpenReport {
  bool created{false};
  bool recovered_torn_tail{false};
  std::uint64_t recovered_bytes{0};
  std::size_t record_count{0};
  RecordSequence last_sequence;
  JournalHeader header;
  /// Deterministic reason codes describing the recovery that happened.
  std::vector<ReasonCode> notes;
};

struct FR_API JournalOpenOptions {
  RuntimeLimits limits{};
  bool create_if_missing{true};
  /// Read-only opens never truncate and never append.
  bool read_only{false};
};

/// One framed durable log.
class FR_API Journal {
 public:
  Journal() = default;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  ~Journal();

  /// Opens or creates the journal at @p path.
  ///
  /// On success, @p report describes any recovery that happened and @p records
  /// contains every intact record in sequence order.
  [[nodiscard]] static Status Open(const std::filesystem::path& path,
                                   const JournalOpenOptions& options,
                                   std::vector<JournalRecord>& records,
                                   JournalOpenReport& report,
                                   Journal& out);

  /// Appends one record and returns only after the durability barrier.
  [[nodiscard]] Status Append(RecordType type, std::string_view payload);

  /// Flushes and fsyncs any buffered bytes.
  [[nodiscard]] Status Sync();

  /// Starts a fresh journal file with the same store identity, replacing the
  /// current file. Used only after a snapshot has been durably published.
  [[nodiscard]] Status Reset(const StoreId& store, std::vector<JournalRecord>& records,
                             JournalOpenReport& report);

  [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] RecordSequence last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_written_; }

  void Close() noexcept;

 private:
  [[nodiscard]] Status WriteHeader(const StoreId& store, RecordSequence base_sequence) noexcept;
  [[nodiscard]] Status WriteAll(const void* data, std::size_t size) noexcept;

  std::filesystem::path path_;
  std::FILE* file_{nullptr};
  RecordSequence last_sequence_;
  std::uint64_t bytes_written_{0};
  RuntimeLimits limits_{};
  bool read_only_{false};
  bool write_failed_{false};
};

/// Builds a complete record image (header plus payload). Exposed so that the
/// adversarial suite can construct corrupt and truncated inputs directly.
[[nodiscard]] FR_API std::string BuildRecordImage(RecordSequence sequence, RecordType type,
                                                  std::string_view payload);

/// Decodes a record header. Returns false, without allocating, when the header
/// is malformed, when the declared payload length exceeds @p max_payload, or
/// when the declared length does not fit in @p available.
[[nodiscard]] FR_API bool DecodeRecordHeader(const std::uint8_t* data, std::size_t available,
                                             std::uint32_t max_payload, RecordType& type,
                                             std::uint32_t& payload_len, RecordSequence& sequence,
                                             std::uint16_t& flags) noexcept;

/// Recomputes the digest of a record image and compares it with the stored
/// value. @p image must contain the whole record.
[[nodiscard]] FR_API bool VerifyRecordImage(std::string_view image) noexcept;

/// Builds the fixed journal header image for a store.
[[nodiscard]] FR_API std::string BuildJournalHeaderImage(const StoreId& store,
                                                         UnixMillis created_unix_ms,
                                                         RecordSequence base_sequence);
[[nodiscard]] FR_API bool ParseJournalHeaderImage(const std::uint8_t* data, std::size_t size,
                                                  JournalHeader& out) noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_JOURNAL_HPP
