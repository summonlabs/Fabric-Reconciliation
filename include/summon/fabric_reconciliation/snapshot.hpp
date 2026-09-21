// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Transactional durable snapshot.
//
// A snapshot is written to a staging file, flushed to disk, and only then moved
// into place with an atomic replacement. A reader therefore observes either
// the previous complete snapshot or the new complete snapshot, never a partial
// one. The snapshot carries the journal sequence watermark it incorporates, so
// replay after load starts exactly after that watermark.

#ifndef SUMMON_FABRIC_RECONCILIATION_SNAPSHOT_HPP
#define SUMMON_FABRIC_RECONCILIATION_SNAPSHOT_HPP

#include <cstdint>
#include <filesystem>
#include <string>

#include "summon/fabric_reconciliation/hash.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"
#include "summon/fabric_reconciliation/version.hpp"

namespace summon {
namespace fabric_reconciliation {

inline constexpr std::size_t kSnapshotHeaderBytes = 112;
inline constexpr std::size_t kSnapshotMagicBytes = 8;
inline constexpr char kSnapshotMagic[kSnapshotMagicBytes + 1] = "FABRSNAP";

struct FR_API SnapshotHeader {
  std::uint32_t format_version{0};
  std::uint32_t header_bytes{0};
  StoreId store;
  RecordSequence watermark;
  CoordinatorEpoch epoch;
  UnixMillis created_unix_ms{0};
  std::uint64_t payload_len{0};
};

[[nodiscard]] FR_API std::string BuildSnapshotImage(const SnapshotHeader& header,
                                                    std::string_view payload);

/// Total parse. Rejects bad magic, unsupported version, wrong header size,
/// impossible payload length, digest mismatch and trailing bytes.
[[nodiscard]] FR_API bool ParseSnapshotImage(std::string_view image, SnapshotHeader& header,
                                             std::string_view& payload) noexcept;

/// Atomically publishes a snapshot. Returns only after the staging file has
/// been flushed and the replacement has completed.
[[nodiscard]] FR_API Status WriteSnapshotAtomic(const std::filesystem::path& path,
                                                const SnapshotHeader& header,
                                                std::string_view payload);

/// Reads and validates a snapshot file. Missing file yields
/// StatusCode::NotFound.
[[nodiscard]] FR_API Status ReadSnapshot(const std::filesystem::path& path,
                                         const RuntimeLimits& limits, SnapshotHeader& header,
                                         std::string& payload);

/// Removes a stale staging file left behind by a crash during publication.
[[nodiscard]] FR_API Status RemoveStagingFile(const std::filesystem::path& path) noexcept;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_SNAPSHOT_HPP
