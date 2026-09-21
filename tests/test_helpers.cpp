// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_helpers.hpp"

#include <atomic>
#include <system_error>

namespace frtest {

namespace fr = summon::fabric_reconciliation;

namespace {

std::atomic<std::uint64_t> g_counter{0};

}  // namespace

ScratchDirectory::ScratchDirectory(const std::string& label) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::uint64_t unique = g_counter.fetch_add(1);
  path_ = base / ("fr-scratch-" + label + "-" + std::to_string(fr::CurrentProcessId()) + "-" +
                  std::to_string(unique));
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
}

ScratchDirectory::~ScratchDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

fr::ScopeId RequireScope(const char* text) {
  const auto parsed = fr::ScopeId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid scope: ") + text);
  }
  return *parsed;
}

fr::SubjectId RequireSubject(const char* text) {
  const auto parsed = fr::SubjectId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid subject: ") + text);
  }
  return *parsed;
}

fr::AttributeKey RequireAttribute(const char* text) {
  const auto parsed = fr::AttributeKey::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid attribute: ") + text);
  }
  return *parsed;
}

fr::ReporterId RequireReporter(const char* text) {
  const auto parsed = fr::ReporterId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid reporter: ") + text);
  }
  return *parsed;
}

fr::ObservationId RequireObservation(const char* text) {
  const auto parsed = fr::ObservationId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid observation: ") + text);
  }
  return *parsed;
}

fr::IntentId RequireIntent(const char* text) {
  const auto parsed = fr::IntentId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid intent: ") + text);
  }
  return *parsed;
}

fr::DefinitionId RequireDefinition(const char* text) {
  const auto parsed = fr::DefinitionId::TryParse(text);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::string("invalid definition: ") + text);
  }
  return *parsed;
}

std::unique_ptr<fr::ReconciliationEngine> OpenEngine(const std::filesystem::path& directory) {
  fr::EngineOptions options;
  options.store_dir = directory;
  options.auto_compact_after_records = 4096;
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status status = fr::ReconciliationEngine::Open(options, engine);
  if (!status.ok()) {
    throw std::runtime_error("engine open failed: " + status.ToString());
  }
  return engine;
}

fr::IntentDocument MakeIntent(const std::string& scope, std::uint64_t generation,
                              std::size_t subjects, fr::EvidenceClass evidence) {
  fr::IntentDocument document;
  document.intent = RequireIntent("intent/test");
  document.scope = RequireScope(scope.c_str());
  document.definition = RequireDefinition("definition/test");
  document.generation = fr::IntentGeneration(generation);
  document.policy_version = fr::PolicyVersion(1);
  document.evidence = evidence;
  document.complete = true;
  const fr::AttributeKey key = RequireAttribute("mtu");
  for (std::size_t index = 0; index < subjects; ++index) {
    const std::string name = "node/n" + std::to_string(index) + "/port/eth0";
    const fr::SubjectId subject = RequireSubject(name.c_str());
    fr::SubjectIntent entry;
    entry.id = subject;
    entry.desired.emplace(key, fr::AttributeValue::Unsigned(9000));
    document.subjects.emplace(subject, std::move(entry));
  }
  document.digest = fr::ComputeIntentDigest(document);
  return document;
}

fr::ObservationSubmission MakeObservation(const std::string& scope, const std::string& reporter,
                                          std::uint64_t generation, std::size_t subjects,
                                          unsigned mismatch_modulus, bool skip_missing,
                                          fr::EvidenceClass evidence) {
  fr::ObservationSubmission submission;
  submission.observation =
      RequireObservation(("observation/" + reporter + "/" + std::to_string(generation)).c_str());
  submission.scope = RequireScope(scope.c_str());
  submission.reporter = RequireReporter(reporter.c_str());
  submission.generation = fr::ObservationGeneration(generation);
  submission.evidence = evidence;
  submission.complete = true;
  submission.authoritative_absence = true;
  const fr::AttributeKey key = RequireAttribute("mtu");
  for (std::size_t index = 0; index < subjects; ++index) {
    if (skip_missing && (index % 17) == 3) {
      continue;
    }
    const std::string name = "node/n" + std::to_string(index) + "/port/eth0";
    const fr::SubjectId subject = RequireSubject(name.c_str());
    fr::SubjectObservation entry;
    entry.id = subject;
    const unsigned value = (mismatch_modulus != 0 && (index % mismatch_modulus) == 0) ? 1500u : 9000u;
    entry.observed.emplace(key, fr::AttributeValue::Unsigned(value));
    submission.subjects.emplace(subject, std::move(entry));
  }
  return submission;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace frtest
