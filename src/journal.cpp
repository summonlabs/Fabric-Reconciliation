// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/journal.hpp"

#include <cstring>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace summon {
namespace fabric_reconciliation {
namespace {

constexpr char kJournalMagic[8] = {'F', 'A', 'B', 'R', 'J', 'N', 'L', '1'};

void StoreU16(std::string& out, std::size_t offset, std::uint16_t value) {
  out[offset] = static_cast<char>(value & 0xffu);
  out[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

void StoreU32(std::string& out, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xffu);
  }
}

void StoreU64(std::string& out, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xffu);
  }
}

std::uint16_t LoadU16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1])
                                                               << 8));
}

std::uint32_t LoadU32(const std::uint8_t* data) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8u * index);
  }
  return value;
}

std::uint64_t LoadU64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8u * index);
  }
  return value;
}

Status FlushToDisk(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                  "flush failed");
  }
#if defined(_WIN32)
  if (::_commit(::_fileno(file)) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                  "commit to disk failed");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                  "fsync failed");
  }
#endif
  return Status::Ok();
}

std::FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  const errno_t error = ::_wfopen_s(&file, path.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
  if (error != 0) {
    return nullptr;
  }
  return file;
#else
  return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

const char* ToText(RecordType value) noexcept {
  switch (value) {
    case RecordType::Invalid: return "INVALID";
    case RecordType::StoreHeader: return "STORE_HEADER";
    case RecordType::PolicyPut: return "POLICY_PUT";
    case RecordType::IntentCommit: return "INTENT_COMMIT";
    case RecordType::ObservationCommit: return "OBSERVATION_COMMIT";
    case RecordType::AttemptIssued: return "ATTEMPT_ISSUED";
    case RecordType::AttemptTransition: return "ATTEMPT_TRANSITION";
    case RecordType::OutcomeCommit: return "OUTCOME_COMMIT";
    case RecordType::FencePut: return "FENCE_PUT";
    case RecordType::FenceClear: return "FENCE_CLEAR";
    case RecordType::EpochAdvance: return "EPOCH_ADVANCE";
    case RecordType::LineageTrim: return "LINEAGE_TRIM";
  }
  return "INVALID";
}

bool TryParseRecordType(std::uint16_t raw, RecordType& out) noexcept {
  if (raw < static_cast<std::uint16_t>(RecordType::StoreHeader) ||
      raw > static_cast<std::uint16_t>(RecordType::LineageTrim)) {
    return false;
  }
  out = static_cast<RecordType>(raw);
  return true;
}

std::string BuildJournalHeaderImage(const StoreId& store, UnixMillis created_unix_ms,
                                    RecordSequence base_sequence) {
  std::string image;
  image.resize(kJournalHeaderBytes, '\0');
  std::memcpy(&image[0], kJournalMagic, sizeof(kJournalMagic));
  StoreU32(image, 8, static_cast<std::uint32_t>(kJournalFormatVersion));
  StoreU32(image, 12, static_cast<std::uint32_t>(kJournalHeaderBytes));
  std::memcpy(&image[16], store.bytes().data(), store.bytes().size());
  StoreU64(image, 32, static_cast<std::uint64_t>(created_unix_ms));
  StoreU64(image, 40, base_sequence.value());
  const Sha256Digest digest =
      Sha256Of(reinterpret_cast<const std::uint8_t*>(image.data()), 64);
  std::memcpy(&image[64], digest.data(), digest.size());
  return image;
}

bool ParseJournalHeaderImage(const std::uint8_t* data, std::size_t size,
                             JournalHeader& out) noexcept {
  if (data == nullptr || size != kJournalHeaderBytes) {
    return false;
  }
  if (std::memcmp(data, kJournalMagic, sizeof(kJournalMagic)) != 0) {
    return false;
  }
  const std::uint32_t version = LoadU32(data + 8);
  const std::uint32_t header_bytes = LoadU32(data + 12);
  if (version != static_cast<std::uint32_t>(kJournalFormatVersion)) {
    return false;
  }
  if (header_bytes != static_cast<std::uint32_t>(kJournalHeaderBytes)) {
    return false;
  }
  for (std::size_t index = 48; index < 64; ++index) {
    if (data[index] != 0) {
      return false;
    }
  }
  const Sha256Digest expected = Sha256Of(data, 64);
  if (std::memcmp(data + 64, expected.data(), expected.size()) != 0) {
    return false;
  }
  std::array<std::uint8_t, 16> store_bytes{};
  std::memcpy(store_bytes.data(), data + 16, store_bytes.size());
  out.format_version = version;
  out.header_bytes = header_bytes;
  out.store = StoreId::FromBytes(store_bytes);
  out.created_unix_ms = static_cast<UnixMillis>(LoadU64(data + 32));
  out.base_sequence = RecordSequence(LoadU64(data + 40));
  return true;
}

std::string BuildRecordImage(RecordSequence sequence, RecordType type, std::string_view payload) {
  std::string image;
  image.resize(kRecordHeaderBytes, '\0');
  StoreU32(image, 0, kRecordMagic);
  StoreU16(image, 4, kRecordFormatVersion);
  StoreU16(image, 6, static_cast<std::uint16_t>(type));
  StoreU16(image, 8, kJournalFlagsNone);
  StoreU16(image, 10, 0);
  StoreU32(image, 12, static_cast<std::uint32_t>(payload.size() & 0xffffffffu));
  StoreU64(image, 16, sequence.value());
  image.append(payload.data(), payload.size());
  Sha256 hasher;
  hasher.Update(reinterpret_cast<const std::uint8_t*>(image.data()), kRecordDigestOffset);
  hasher.Update(payload.data(), payload.size());
  const Sha256Digest digest = hasher.Final();
  std::memcpy(&image[kRecordDigestOffset], digest.data(), digest.size());
  return image;
}

bool DecodeRecordHeader(const std::uint8_t* data, std::size_t available,
                        std::uint32_t max_payload, RecordType& type, std::uint32_t& payload_len,
                        RecordSequence& sequence, std::uint16_t& flags) noexcept {
  if (data == nullptr || available < kRecordHeaderBytes) {
    return false;
  }
  if (LoadU32(data) != kRecordMagic) {
    return false;
  }
  if (LoadU16(data + 4) != kRecordFormatVersion) {
    return false;
  }
  RecordType parsed_type = RecordType::Invalid;
  if (!TryParseRecordType(LoadU16(data + 6), parsed_type)) {
    return false;
  }
  const std::uint16_t parsed_flags = LoadU16(data + 8);
  if (parsed_flags != kJournalFlagsNone) {
    return false;
  }
  if (LoadU16(data + 10) != 0) {
    return false;
  }
  const std::uint32_t parsed_length = LoadU32(data + 12);
  if (parsed_length > max_payload) {
    return false;
  }
  type = parsed_type;
  payload_len = parsed_length;
  flags = parsed_flags;
  sequence = RecordSequence(LoadU64(data + 16));
  return true;
}

bool VerifyRecordImage(std::string_view image) noexcept {
  if (image.size() < kRecordHeaderBytes) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
  if (LoadU32(bytes) != kRecordMagic) {
    return false;
  }
  const std::uint32_t payload_len = LoadU32(bytes + 12);
  if (image.size() != kRecordHeaderBytes + static_cast<std::size_t>(payload_len)) {
    return false;
  }
  Sha256 hasher;
  hasher.Update(bytes, kRecordDigestOffset);
  hasher.Update(bytes + kRecordHeaderBytes, payload_len);
  const Sha256Digest digest = hasher.Final();
  return std::memcmp(bytes + kRecordDigestOffset, digest.data(), digest.size()) == 0;
}

Journal::Journal(Journal&& other) noexcept
    : path_(std::move(other.path_)),
      file_(other.file_),
      last_sequence_(other.last_sequence_),
      bytes_written_(other.bytes_written_),
      limits_(other.limits_),
      read_only_(other.read_only_),
      write_failed_(other.write_failed_) {
  other.file_ = nullptr;
  other.bytes_written_ = 0;
}

Journal& Journal::operator=(Journal&& other) noexcept {
  if (this != &other) {
    Close();
    path_ = std::move(other.path_);
    file_ = other.file_;
    last_sequence_ = other.last_sequence_;
    bytes_written_ = other.bytes_written_;
    limits_ = other.limits_;
    read_only_ = other.read_only_;
    write_failed_ = other.write_failed_;
    other.file_ = nullptr;
    other.bytes_written_ = 0;
  }
  return *this;
}

Journal::~Journal() { Close(); }

void Journal::Close() noexcept {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

Status Journal::WriteAll(const void* data, std::size_t size) noexcept {
  if (file_ == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "journal is not open");
  }
  if (write_failed_) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                  "journal write previously failed");
  }
  if (size == 0) {
    return Status::Ok();
  }
  const std::size_t written = std::fwrite(data, 1, size, file_);
  if (written != size) {
    write_failed_ = true;
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                  "short write on journal");
  }
  bytes_written_ += static_cast<std::uint64_t>(size);
  return Status::Ok();
}

Status Journal::WriteHeader(const StoreId& store, RecordSequence base_sequence) noexcept {
  const std::string image = BuildJournalHeaderImage(store, WallClockMillis(), base_sequence);
  return WriteAll(image.data(), image.size());
}

Status Journal::Open(const std::filesystem::path& path, const JournalOpenOptions& options,
                     std::vector<JournalRecord>& records, JournalOpenReport& report, Journal& out) {
  out.Close();
  out.path_ = path;
  out.limits_ = options.limits;
  out.read_only_ = options.read_only;
  out.write_failed_ = false;
  out.last_sequence_ = RecordSequence(0);
  out.bytes_written_ = 0;
  records.clear();
  report = JournalOpenReport{};

  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "cannot stat journal path");
  }

  if (!exists) {
    if (!options.create_if_missing) {
      return Status(StatusCode::NotFound, ReasonCode::DurableWriteFailed,
                    "journal does not exist");
    }
    const StoreId store = StoreId::Generate();
    out.file_ = OpenFile(path, "wb+");
    if (out.file_ == nullptr) {
      return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                    "cannot create journal");
    }
    Status status = out.WriteHeader(store, RecordSequence(0));
    if (!status.ok()) {
      out.Close();
      return status;
    }
    status = FlushToDisk(out.file_);
    if (!status.ok()) {
      out.Close();
      return status;
    }
    report.created = true;
    report.header.store = store;
    report.header.format_version = kJournalFormatVersion;
    report.header.header_bytes = static_cast<std::uint32_t>(kJournalHeaderBytes);
    report.header.created_unix_ms = WallClockMillis();
    report.header.base_sequence = RecordSequence(0);
    report.last_sequence = RecordSequence(0);
    return Status::Ok();
  }

  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed,
                  "cannot size journal file");
  }
  if (size > options.limits.max_journal_bytes) {
    return Status(StatusCode::LimitExceeded, ReasonCode::JournalPayloadLengthInvalid,
                  "journal exceeds the configured maximum size");
  }

  std::FILE* handle = OpenFile(path, options.read_only ? "rb" : "rb+");
  if (handle == nullptr) {
    return Status(StatusCode::Rejected, ReasonCode::DurableWriteFailed, "cannot open journal");
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t read = std::fread(&content[0], 1, static_cast<std::size_t>(size), handle);
    if (read != static_cast<std::size_t>(size)) {
      std::fclose(handle);
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "short read on journal");
    }
  }

  if (content.size() < kJournalHeaderBytes) {
    std::fclose(handle);
    return Status(StatusCode::IntegrityFailure, ReasonCode::JournalHeaderInvalid,
                  "journal is shorter than its fixed header");
  }
  JournalHeader header;
  if (!ParseJournalHeaderImage(reinterpret_cast<const std::uint8_t*>(content.data()),
                               kJournalHeaderBytes, header)) {
    std::fclose(handle);
    return Status(StatusCode::IntegrityFailure, ReasonCode::JournalHeaderInvalid,
                  "journal header failed validation");
  }
  out.file_ = handle;
  report.header = header;

  std::size_t offset = kJournalHeaderBytes;
  RecordSequence expected(header.base_sequence.value() + 1);
  std::size_t torn_offset = content.size();
  bool torn = false;
  while (offset < content.size()) {
    const std::size_t remaining = content.size() - offset;
    const auto* base = reinterpret_cast<const std::uint8_t*>(content.data()) + offset;
    if (remaining < kRecordDigestOffset) {
      torn = true;
      torn_offset = offset;
      break;
    }
    if (LoadU32(base) != kRecordMagic) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalTrailingGarbage,
                    "journal contains a complete record header region without a record magic");
    }
    RecordType type = RecordType::Invalid;
    std::uint32_t payload_len = 0;
    RecordSequence sequence;
    std::uint16_t flags = 0;
    if (!DecodeRecordHeader(base, remaining, static_cast<std::uint32_t>(
                                                   options.limits.max_journal_payload_bytes),
                            type, payload_len, sequence, flags)) {
      // The header is present but unusable. This is only a torn tail when the
      // bytes cannot describe a full record at all.
      if (remaining < kRecordHeaderBytes) {
        torn = true;
        torn_offset = offset;
        break;
      }
      const std::uint32_t declared = LoadU32(base + 12);
      if (declared > options.limits.max_journal_payload_bytes) {
        out.Close();
        return Status(StatusCode::IntegrityFailure, ReasonCode::JournalPayloadLengthInvalid,
                      "journal record declares an impossible payload length");
      }
      if (remaining < kRecordHeaderBytes + static_cast<std::size_t>(declared)) {
        torn = true;
        torn_offset = offset;
        break;
      }
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalRecordTypeUnknown,
                    "journal record header is invalid");
    }
    if (remaining < kRecordHeaderBytes + static_cast<std::size_t>(payload_len)) {
      torn = true;
      torn_offset = offset;
      break;
    }
    const std::string_view image(content.data() + offset, kRecordHeaderBytes + payload_len);
    if (!VerifyRecordImage(image)) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalRecordDigestMismatch,
                    "journal record failed its integrity digest");
    }
    if (!(sequence == expected)) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalSequenceRegression,
                    "journal record sequence is not contiguous");
    }
    JournalRecord record;
    record.sequence = sequence;
    record.type = type;
    record.payload.assign(image.data() + kRecordHeaderBytes, payload_len);
    records.push_back(std::move(record));
    expected = RecordSequence(sequence.value() + 1);
    offset += kRecordHeaderBytes + payload_len;
  }

  if (torn) {
    if (options.read_only) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::JournalTornTailRecovered,
                    "journal has a torn tail and was opened read-only");
    }
    if (std::fflush(handle) != 0) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "cannot flush journal before truncation");
    }
#if defined(_WIN32)
    if (::_chsize_s(::_fileno(handle), static_cast<__int64>(torn_offset)) != 0) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "cannot truncate torn tail");
    }
#else
    if (::ftruncate(::fileno(handle), static_cast<off_t>(torn_offset)) != 0) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "cannot truncate torn tail");
    }
#endif
    if (std::fseek(handle, 0, SEEK_END) != 0) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "cannot seek after truncation");
    }
    report.recovered_torn_tail = true;
    report.recovered_bytes = static_cast<std::uint64_t>(content.size() - torn_offset);
    report.notes.push_back(ReasonCode::JournalTornTailRecovered);
  } else if (!options.read_only) {
    if (std::fseek(handle, 0, SEEK_END) != 0) {
      out.Close();
      return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed,
                    "cannot seek to journal end");
    }
  }

  out.bytes_written_ = static_cast<std::uint64_t>(torn ? torn_offset : content.size());
  out.last_sequence_ =
      records.empty() ? header.base_sequence : records.back().sequence;
  report.record_count = records.size();
  report.last_sequence = out.last_sequence_;
  report.header = header;
  return Status::Ok();
}

Status Journal::Append(RecordType type, std::string_view payload) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "journal is open read-only");
  }
  if (file_ == nullptr) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "journal is not open");
  }
  if (payload.size() > limits_.max_journal_payload_bytes) {
    return Status(StatusCode::LimitExceeded, ReasonCode::JournalPayloadLengthInvalid,
                  "record payload exceeds the configured bound");
  }
  const RecordSequence next(last_sequence_.value() + 1);
  const std::string image = BuildRecordImage(next, type, payload);
  Status status = WriteAll(image.data(), image.size());
  if (!status.ok()) {
    return status;
  }
  status = FlushToDisk(file_);
  if (!status.ok()) {
    write_failed_ = true;
    return status;
  }
  last_sequence_ = next;
  return Status::Ok();
}

Status Journal::Sync() {
  if (file_ == nullptr || read_only_) {
    return Status::Ok();
  }
  return FlushToDisk(file_);
}

Status Journal::Reset(const StoreId& store, std::vector<JournalRecord>& records,
                      JournalOpenReport& report) {
  if (read_only_) {
    return Status(StatusCode::PreconditionFailed, ReasonCode::DurableWriteFailed,
                  "journal is open read-only");
  }
  const RecordSequence carry = last_sequence_;
  Close();
  records.clear();
  report = JournalOpenReport{};
  file_ = OpenFile(path_, "wb+");
  if (file_ == nullptr) {
    return Status(StatusCode::Rejected, ReasonCode::SnapshotReplacementFailed,
                  "cannot recreate journal");
  }
  Status status = WriteHeader(store, carry);
  if (!status.ok()) {
    Close();
    return status;
  }
  status = FlushToDisk(file_);
  if (!status.ok()) {
    Close();
    return status;
  }
  last_sequence_ = carry;
  bytes_written_ = kJournalHeaderBytes;
  report.created = true;
  report.last_sequence = carry;
  report.header.store = store;
  report.header.format_version = kJournalFormatVersion;
  report.header.header_bytes = static_cast<std::uint32_t>(kJournalHeaderBytes);
  report.header.created_unix_ms = WallClockMillis();
  report.header.base_sequence = carry;
  return Status::Ok();
}

}  // namespace fabric_reconciliation
}  // namespace summon
