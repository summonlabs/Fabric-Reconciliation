// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/snapshot.hpp"

#include <cstdio>
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

Status FlushToDisk(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed, "flush failed");
  }
#if defined(_WIN32)
  if (::_commit(::_fileno(file)) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed, "commit failed");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::DurableWriteFailed, "fsync failed");
  }
#endif
  return Status::Ok();
}

}  // namespace

std::string BuildSnapshotImage(const SnapshotHeader& header, std::string_view payload) {
  std::string image;
  image.resize(kSnapshotHeaderBytes, '\0');
  std::memcpy(&image[0], kSnapshotMagic, kSnapshotMagicBytes);
  StoreU32(image, 8, header.format_version);
  StoreU32(image, 12, header.header_bytes);
  std::memcpy(&image[16], header.store.bytes().data(), header.store.bytes().size());
  StoreU64(image, 32, header.watermark.value());
  StoreU64(image, 40, header.epoch.value());
  StoreU64(image, 48, static_cast<std::uint64_t>(header.created_unix_ms));
  StoreU64(image, 56, static_cast<std::uint64_t>(payload.size()));
  Sha256 hasher;
  hasher.Update(reinterpret_cast<const std::uint8_t*>(image.data()), 80);
  hasher.Update(payload.data(), payload.size());
  const Sha256Digest digest = hasher.Final();
  std::memcpy(&image[80], digest.data(), digest.size());
  image.append(payload.data(), payload.size());
  return image;
}

bool ParseSnapshotImage(std::string_view image, SnapshotHeader& header,
                        std::string_view& payload) noexcept {
  if (image.size() < kSnapshotHeaderBytes) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
  if (std::memcmp(bytes, kSnapshotMagic, kSnapshotMagicBytes) != 0) {
    return false;
  }
  const std::uint32_t version = LoadU32(bytes + 8);
  const std::uint32_t header_bytes = LoadU32(bytes + 12);
  if (version != static_cast<std::uint32_t>(kSnapshotFormatVersion)) {
    return false;
  }
  if (header_bytes != static_cast<std::uint32_t>(kSnapshotHeaderBytes)) {
    return false;
  }
  for (std::size_t index = 64; index < 80; ++index) {
    if (bytes[index] != 0) {
      return false;
    }
  }
  const std::uint64_t payload_len = LoadU64(bytes + 56);
  if (image.size() - kSnapshotHeaderBytes != static_cast<std::size_t>(payload_len)) {
    return false;
  }
  Sha256 hasher;
  hasher.Update(bytes, 80);
  hasher.Update(bytes + kSnapshotHeaderBytes, static_cast<std::size_t>(payload_len));
  const Sha256Digest digest = hasher.Final();
  if (std::memcmp(bytes + 80, digest.data(), digest.size()) != 0) {
    return false;
  }
  std::array<std::uint8_t, 16> store_bytes{};
  std::memcpy(store_bytes.data(), bytes + 16, store_bytes.size());
  header.format_version = version;
  header.header_bytes = header_bytes;
  header.store = StoreId::FromBytes(store_bytes);
  header.watermark = RecordSequence(LoadU64(bytes + 32));
  header.epoch = CoordinatorEpoch(LoadU64(bytes + 40));
  header.created_unix_ms = static_cast<UnixMillis>(LoadU64(bytes + 48));
  header.payload_len = payload_len;
  payload = image.substr(kSnapshotHeaderBytes, static_cast<std::size_t>(payload_len));
  return true;
}

Status WriteSnapshotAtomic(const std::filesystem::path& path, const SnapshotHeader& header,
                           std::string_view payload) {
  const std::filesystem::path staging = path.string() + ".staging";
  std::error_code error;
  std::filesystem::remove(staging, error);
  std::FILE* file = OpenFile(staging, "wb");
  if (file == nullptr) {
    return Status(StatusCode::Rejected, ReasonCode::SnapshotReplacementFailed,
                  "cannot create snapshot staging file");
  }
  const std::string image = BuildSnapshotImage(header, payload);
  const std::size_t written = std::fwrite(image.data(), 1, image.size(), file);
  if (written != image.size()) {
    std::fclose(file);
    std::filesystem::remove(staging, error);
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotReplacementFailed,
                  "short write on snapshot staging file");
  }
  const Status flush = FlushToDisk(file);
  std::fclose(file);
  if (!flush.ok()) {
    std::filesystem::remove(staging, error);
    return flush;
  }
  std::filesystem::rename(staging, path, error);
  if (error) {
    std::filesystem::remove(staging, error);
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotReplacementFailed,
                  "atomic snapshot replacement failed");
  }
  return Status::Ok();
}

Status ReadSnapshot(const std::filesystem::path& path, const RuntimeLimits& limits,
                    SnapshotHeader& header, std::string& payload) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return Status(StatusCode::NotFound, ReasonCode::None, "snapshot does not exist");
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return Status(StatusCode::Rejected, ReasonCode::SnapshotDigestMismatch,
                  "cannot size snapshot file");
  }
  const std::size_t max_bytes = limits.max_journal_bytes + kSnapshotHeaderBytes;
  if (size > max_bytes) {
    return Status(StatusCode::LimitExceeded, ReasonCode::SnapshotDigestMismatch,
                  "snapshot exceeds the configured maximum size");
  }
  std::FILE* file = OpenFile(path, "rb");
  if (file == nullptr) {
    return Status(StatusCode::Rejected, ReasonCode::SnapshotDigestMismatch,
                  "cannot open snapshot file");
  }
  std::string content;
  content.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    const std::size_t read = std::fread(&content[0], 1, static_cast<std::size_t>(size), file);
    if (read != static_cast<std::size_t>(size)) {
      std::fclose(file);
      return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                    "short read on snapshot file");
    }
  }
  std::fclose(file);

  std::string_view view;
  if (!ParseSnapshotImage(content, header, view)) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                  "snapshot failed validation");
  }
  payload.assign(view.data(), view.size());
  return Status::Ok();
}

Status RemoveStagingFile(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const std::filesystem::path staging = path.string() + ".staging";
  std::filesystem::remove(staging, error);
  if (error) {
    return Status(StatusCode::IntegrityFailure, ReasonCode::SnapshotReplacementFailed,
                  "cannot remove snapshot staging file");
  }
  return Status::Ok();
}

}  // namespace fabric_reconciliation
}  // namespace summon
