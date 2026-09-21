// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Product identity for the Fabric Reconciliation runtime.

#ifndef SUMMON_FABRIC_RECONCILIATION_VERSION_HPP
#define SUMMON_FABRIC_RECONCILIATION_VERSION_HPP

#include <cstdint>

namespace summon {
namespace fabric_reconciliation {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Semantic version of the Fabric Reconciliation runtime.
inline constexpr const char* kVersionString = "1.0.0";
inline constexpr const char* kProductName = "Fabric Reconciliation";
inline constexpr const char* kProductVendor = "Summon Software Labs";

/// Durable format versions. Every persisted artifact declares one of these and
/// every reader refuses a version it does not understand.
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;
inline constexpr std::uint16_t kCanonicalSchemaVersion = 1;
inline constexpr std::uint16_t kWireProtocolVersion = 1;

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_VERSION_HPP
