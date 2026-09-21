// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared deterministic helpers for the test suites.
//
// Every helper here is seeded and reproducible. There is no sleeping, no
// timeout and no reliance on scheduler luck anywhere in the suites.

#ifndef SUMMON_FABRIC_RECONCILIATION_TEST_HELPERS_HPP
#define SUMMON_FABRIC_RECONCILIATION_TEST_HELPERS_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/document_json.hpp"
#include "summon/fabric_reconciliation/engine.hpp"

namespace frtest {

/// Deterministic xorshift64* generator. Seeded explicitly by every caller.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

  std::uint64_t Next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 2685821657736338717ull;
  }

  std::uint64_t Below(std::uint64_t bound) { return bound == 0 ? 0 : (Next() % bound); }

  bool Coin() { return (Next() & 1ull) != 0; }

 private:
  std::uint64_t state_;
};

/// A unique scratch directory removed by its destructor.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& label);
  ~ScratchDirectory();

  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path child(const std::string& name) const {
    return path_ / name;
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] summon::fabric_reconciliation::ScopeId RequireScope(const char* text);
[[nodiscard]] summon::fabric_reconciliation::SubjectId RequireSubject(const char* text);
[[nodiscard]] summon::fabric_reconciliation::AttributeKey RequireAttribute(const char* text);
[[nodiscard]] summon::fabric_reconciliation::ReporterId RequireReporter(const char* text);
[[nodiscard]] summon::fabric_reconciliation::ObservationId RequireObservation(const char* text);
[[nodiscard]] summon::fabric_reconciliation::IntentId RequireIntent(const char* text);
[[nodiscard]] summon::fabric_reconciliation::DefinitionId RequireDefinition(const char* text);

/// Builds an engine rooted at @p directory.
[[nodiscard]] std::unique_ptr<summon::fabric_reconciliation::ReconciliationEngine> OpenEngine(
    const std::filesystem::path& directory);

/// Builds a synthetic intent with @p subjects subjects named node/nK/port/eth0,
/// each declaring mtu=9000.
[[nodiscard]] summon::fabric_reconciliation::IntentDocument MakeIntent(
    const std::string& scope, std::uint64_t generation, std::size_t subjects,
    summon::fabric_reconciliation::EvidenceClass evidence);

/// Builds a synthetic observation covering @p subjects subjects. Subject index
/// K observes mtu = (K % modulus == 0) ? 1500 : 9000; when @p skip_missing is
/// true every 17th subject is omitted.
[[nodiscard]] summon::fabric_reconciliation::ObservationSubmission MakeObservation(
    const std::string& scope, const std::string& reporter, std::uint64_t generation,
    std::size_t subjects, unsigned mismatch_modulus, bool skip_missing,
    summon::fabric_reconciliation::EvidenceClass evidence);

/// Case-insensitive search used by the assertions that check rendered output.
[[nodiscard]] bool Contains(const std::string& haystack, const std::string& needle);

}  // namespace frtest

#endif  // SUMMON_FABRIC_RECONCILIATION_TEST_HELPERS_HPP
