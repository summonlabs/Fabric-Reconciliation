// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Command line surface for the Fabric Reconciliation runtime.
//
// Every command operates on a real durable store through the same public API
// the library exposes. Nothing here reaches into private state, and nothing
// here invents behaviour the library does not have.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "summon/fabric_reconciliation/document_json.hpp"
#include "summon/fabric_reconciliation/engine.hpp"
#include "summon/fabric_reconciliation/server.hpp"
#include "summon/fabric_reconciliation/version.hpp"

namespace fr = summon::fabric_reconciliation;

namespace {

int Fail(const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  return 1;
}

struct Options {
  std::string command;
  std::string store{"fabric-store"};
  std::string scope;
  std::string subject;
  std::string file;
  std::string out;
  std::string address{"127.0.0.1"};
  std::string plan_id;
  std::string state;
  std::string key;
  std::string ready_file;
  long long now{0};
  long long port{0};
  long long attempt{0};
  long long below{0};
  bool json{false};
  bool dry_run{false};
  bool quiet{false};
  bool stdin_control{false};
};

std::string ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

int WriteFile(const std::string& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return 1;
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  return stream ? 0 : 1;
}

fr::Status OpenEngine(const Options& options, std::unique_ptr<fr::ReconciliationEngine>& engine) {
  fr::EngineOptions engine_options;
  engine_options.store_dir = std::filesystem::path(options.store);
  return fr::ReconciliationEngine::Open(engine_options, engine);
}

fr::Status CommitPolicy(const Options& options, fr::ReconciliationEngine& engine) {
  fr::ReconciliationPolicy policy = fr::DefaultPolicy();
  if (!options.file.empty()) {
    const std::string text = ReadFile(options.file);
    if (text.empty()) {
      return fr::Status(fr::StatusCode::Rejected, fr::ReasonCode::MalformedPayload,
                        "policy file is empty or unreadable");
    }
    const fr::Status parsed = fr::ParseJson(text, policy, fr::RuntimeLimits{});
    if (!parsed.ok()) {
      return parsed;
    }
  }
  return engine.PutPolicy(policy, true);
}

int CommandInit(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  if (!options.quiet) {
    std::printf("%s", fr::RenderBootReportText(engine->boot_report()).c_str());
  }
  return 0;
}

int CommandStatus(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  if (options.json) {
    std::printf("%s\n", fr::ToJson(engine->boot_report()).c_str());
    return 0;
  }
  std::printf("%s", fr::RenderBootReportText(engine->boot_report()).c_str());
  const auto policy = engine->ActivePolicy();
  if (policy.ok()) {
    std::printf("%s", fr::ToJson(*policy).c_str());
    std::printf("\n");
  }
  const auto scopes = engine->KnownScopes();
  if (scopes.ok()) {
    for (const fr::ScopeId& scope : *scopes) {
      std::printf("scope %s\n", scope.str().c_str());
    }
  }
  return 0;
}

int CommandPolicy(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  const fr::Status committed = CommitPolicy(options, *engine);
  if (!committed.ok()) {
    return Fail(committed.ToString());
  }
  const auto policy = engine->ActivePolicy();
  if (!policy.ok()) {
    return Fail(policy.status().ToString());
  }
  std::printf("%s\n", fr::ToJson(*policy).c_str());
  return 0;
}

int CommandIntent(const Options& options) {
  const std::string text = ReadFile(options.file);
  if (text.empty()) {
    return Fail("intent file is empty or unreadable");
  }
  fr::IntentDocument document;
  const fr::Status parsed = fr::ParseJson(text, document, fr::RuntimeLimits{});
  if (!parsed.ok()) {
    return Fail(parsed.ToString());
  }
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::IntentCommitResult result;
  const fr::Status committed = engine->CommitIntent(document, result);
  if (!committed.ok()) {
    return Fail(committed.ToString());
  }
  std::printf("intent scope=%s generation=%llu duplicate=%s digest=%s\n",
              document.scope.str().c_str(),
              static_cast<unsigned long long>(result.generation.value()),
              result.duplicate ? "yes" : "no", fr::ToHex(result.digest).c_str());
  return 0;
}

int CommandObserve(const Options& options) {
  const std::string text = ReadFile(options.file);
  if (text.empty()) {
    return Fail("observation file is empty or unreadable");
  }
  fr::ObservationSubmission submission;
  const fr::Status parsed = fr::ParseJson(text, submission, fr::RuntimeLimits{});
  if (!parsed.ok()) {
    return Fail(parsed.ToString());
  }
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::ObservationCommitResult result;
  const fr::Status committed = engine->RecordObservationAt(
      submission, static_cast<fr::UnixMillis>(options.now), result);
  if (!committed.ok()) {
    return Fail(committed.ToString());
  }
  if (options.json) {
    std::printf("%s\n", fr::ToJson(result.stamped).c_str());
    return 0;
  }
  std::printf("observation scope=%s reporter=%s generation=%llu duplicate=%s epoch=%llu\n",
              submission.scope.str().c_str(), submission.reporter.str().c_str(),
              static_cast<unsigned long long>(submission.generation.value()),
              result.duplicate ? "yes" : "no",
              static_cast<unsigned long long>(result.stamped.received_epoch.value()));
  return 0;
}

int CommandPlan(const Options& options, bool explain_only) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  const auto scope = fr::ScopeId::TryParse(options.scope);
  if (!scope.has_value()) {
    return Fail("--scope is required and must be a valid identity");
  }
  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = static_cast<fr::UnixMillis>(options.now);
  request.dry_run = options.dry_run;
  const auto plan = engine->Plan(request);
  if (!plan.ok()) {
    return Fail(plan.status().ToString());
  }
  if (options.json) {
    std::printf("%s\n", fr::ToJson(*plan).c_str());
  } else if (explain_only) {
    std::printf("%s", fr::RenderPlanText(*plan).c_str());
  } else {
    std::printf("%s", fr::RenderPlanText(*plan).c_str());
  }
  if (!options.out.empty()) {
    const int written = WriteFile(options.out, fr::ToJson(*plan) + "\n");
    if (written != 0) {
      return Fail("could not write the plan document");
    }
  }
  return 0;
}

int CommandClassify(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  const auto scope = fr::ScopeId::TryParse(options.scope);
  if (!scope.has_value()) {
    return Fail("--scope is required and must be a valid identity");
  }
  const auto report = engine->Classify(*scope, static_cast<fr::UnixMillis>(options.now));
  if (!report.ok()) {
    return Fail(report.status().ToString());
  }
  if (options.json) {
    std::printf("%s\n", fr::ToJson(*report).c_str());
  } else {
    std::printf("%s", fr::RenderClassificationText(*report).c_str());
  }
  return 0;
}

int CommandDispatch(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  const auto scope = fr::ScopeId::TryParse(options.scope);
  if (!scope.has_value()) {
    return Fail("--scope is required and must be a valid identity");
  }
  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = static_cast<fr::UnixMillis>(options.now);
  const auto plan = engine->Plan(request);
  if (!plan.ok()) {
    return Fail(plan.status().ToString());
  }
  if (!options.plan_id.empty() && options.plan_id != plan->plan_id) {
    return Fail("the plan changed since the supplied plan id was observed");
  }
  const auto dispatched = engine->Dispatch(*plan);
  if (!dispatched.ok()) {
    return Fail(dispatched.status().ToString());
  }
  std::printf("plan=%s verdict=%s issued=%zu already-issued=%zu fenced=%zu\n",
              plan->plan_id.c_str(), fr::ToText(plan->verdict), dispatched->intents.size(),
              dispatched->already_issued, dispatched->fenced);
  for (const fr::ActionIntent& action : dispatched->intents) {
    std::printf("attempt=%llu key=%s subject=%s action=%s\n",
                static_cast<unsigned long long>(action.attempt.value()),
                fr::ToHex(action.idempotency_key).c_str(), action.subject.str().c_str(),
                fr::ToText(action.action));
  }
  return 0;
}

int CommandComplete(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::AttemptState target = fr::AttemptState::Invalid;
  if (!fr::TryParseAttemptState(options.state.c_str(), target)) {
    return Fail("--state must be one of ACKNOWLEDGED, APPLIED, FAILED, ABANDONED");
  }
  fr::CompletionRequest request;
  request.attempt = fr::AttemptId(static_cast<std::uint64_t>(options.attempt));
  request.epoch = engine->epoch();
  request.boot = engine->boot();
  if (!fr::TryParseHexDigest(options.key, request.idempotency_key)) {
    return Fail("--key must be 64 hexadecimal characters");
  }
  request.target = target;
  request.at_unix_ms = static_cast<fr::UnixMillis>(options.now);
  fr::ReconciliationOutcome outcome;
  const fr::Status completed = engine->Complete(request, outcome);
  if (!completed.ok()) {
    return Fail(completed.ToString());
  }
  std::printf("%s\n", fr::ToJson(outcome).c_str());
  return 0;
}

int CommandVerify(const Options& options) {
  const std::string text = ReadFile(options.file);
  if (text.empty()) {
    return Fail("verification file is empty or unreadable");
  }
  fr::VerificationEvidence evidence;
  const fr::Status parsed = fr::ParseJson(text, evidence, fr::RuntimeLimits{});
  if (!parsed.ok()) {
    return Fail(parsed.ToString());
  }
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::ReconciliationOutcome outcome;
  const fr::Status verified = engine->Verify(evidence, outcome);
  if (!verified.ok()) {
    return Fail(verified.ToString());
  }
  std::printf("%s\n", fr::ToJson(outcome).c_str());
  return 0;
}

int CommandFence(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::FenceEntry fence;
  const auto id = fr::FenceId::TryParse("fence/cli/" + std::to_string(options.below));
  const auto scope = fr::ScopeId::TryParse(options.scope);
  if (!id.has_value() || !scope.has_value()) {
    return Fail("--scope is required and must be a valid identity");
  }
  fence.id = *id;
  fence.scope = *scope;
  if (!options.subject.empty()) {
    const auto subject = fr::SubjectId::TryParse(options.subject);
    if (!subject.has_value()) {
      return Fail("--subject must be a valid identity");
    }
    fence.subject = *subject;
  }
  fence.fenced_below = fr::CoordinatorEpoch(static_cast<std::uint64_t>(options.below));
  fence.reason = fr::ReasonCode::ScopeFenced;
  fence.detail = "operator fence";
  const fr::Status status = engine->PutFence(fence);
  if (!status.ok()) {
    return Fail(status.ToString());
  }
  std::printf("%s\n", fr::ToJson(fence).c_str());
  return 0;
}

int CommandAttempts(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::ScopeId scope;
  if (!options.scope.empty()) {
    const auto parsed = fr::ScopeId::TryParse(options.scope);
    if (!parsed.has_value()) {
      return Fail("--scope must be a valid identity");
    }
    scope = *parsed;
  }
  const auto attempts = engine->Attempts(scope);
  if (!attempts.ok()) {
    return Fail(attempts.status().ToString());
  }
  for (const fr::AttemptRecord& attempt : *attempts) {
    if (options.json) {
      std::printf("%s\n", fr::ToJson(attempt).c_str());
    } else {
      std::printf("%s", fr::RenderAttemptText(attempt).c_str());
    }
  }
  return 0;
}

int CommandOutcomes(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::ScopeId scope;
  if (!options.scope.empty()) {
    const auto parsed = fr::ScopeId::TryParse(options.scope);
    if (!parsed.has_value()) {
      return Fail("--scope must be a valid identity");
    }
    scope = *parsed;
  }
  const auto outcomes = engine->Outcomes(scope);
  if (!outcomes.ok()) {
    return Fail(outcomes.status().ToString());
  }
  for (const fr::ReconciliationOutcome& outcome : *outcomes) {
    std::printf("%s\n", fr::ToJson(outcome).c_str());
  }
  return 0;
}

int CommandServe(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  fr::ServerOptions server_options;
  server_options.bind_address = options.address;
  server_options.port = static_cast<std::uint16_t>(options.port);
  std::unique_ptr<fr::ReconciliationServer> server;
  const fr::Status started = fr::ReconciliationServer::Start(server_options, *engine, server);
  if (!started.ok()) {
    return Fail(started.ToString());
  }
  std::printf("listening %s epoch=%llu boot=%s\n", server->endpoint().c_str(),
              static_cast<unsigned long long>(engine->epoch().value()),
              engine->boot().ToHex().c_str());
  std::fflush(stdout);
  if (!options.ready_file.empty()) {
    // Publishing a readiness file makes "the service is accepting" an explicit
    // observable event rather than something a caller has to guess at by
    // retrying connections.
    const std::string marker = server->endpoint() + "\nepoch=" +
                               std::to_string(engine->epoch().value()) + "\nboot=" +
                               engine->boot().ToHex() + "\n";
    if (WriteFile(options.ready_file, marker) != 0) {
      return Fail("could not write the readiness file");
    }
  }
  if (options.stdin_control) {
    // Opt-in control channel: reads one command per line from standard input.
    // Without it the service never touches standard input, which keeps it
    // usable as a fixture whose standard input is closed or redirected.
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "stop") {
        break;
      }
      if (line == "status") {
        std::printf("accepted=%llu rejected=%llu epoch=%llu\n",
                    static_cast<unsigned long long>(server->connections_accepted()),
                    static_cast<unsigned long long>(server->frames_rejected()),
                    static_cast<unsigned long long>(engine->epoch().value()));
        std::fflush(stdout);
      }
    }
  } else {
    // Runs until the process is terminated.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  const fr::Status stopped = server->Stop();
  if (!stopped.ok()) {
    return Fail(stopped.ToString());
  }
  std::printf("stopped\n");
  return 0;
}

/// Deterministic synthetic end-to-end fixture.
///
/// Everything this command produces is labelled SYNTHETIC. It exercises the
/// complete path from persisted intended and observed state to a deterministic
/// reconciliation plan without touching any physical fabric element.
int CommandScenario(const Options& options) {
  std::unique_ptr<fr::ReconciliationEngine> engine;
  const fr::Status opened = OpenEngine(options, engine);
  if (!opened.ok()) {
    return Fail(opened.ToString());
  }
  const auto scope = fr::ScopeId::TryParse("fabric/rack-7");
  const auto intent_id = fr::IntentId::TryParse("intent/rack-7");
  const auto definition = fr::DefinitionId::TryParse("definition/rack-7");
  const auto observer = fr::ReporterId::TryParse("reporter/synthetic-probe");
  const auto observation_id = fr::ObservationId::TryParse("observation/rack-7/1");
  if (!scope.has_value() || !intent_id.has_value() || !definition.has_value() ||
      !observer.has_value() || !observation_id.has_value()) {
    return Fail("synthetic fixture identities are invalid");
  }

  fr::IntentDocument intent;
  intent.intent = *intent_id;
  intent.scope = *scope;
  intent.definition = *definition;
  intent.generation = fr::IntentGeneration(1);
  intent.policy_version = fr::PolicyVersion(1);
  intent.evidence = fr::EvidenceClass::Synthetic;
  intent.complete = true;

  fr::ObservationSubmission observation;
  observation.observation = *observation_id;
  observation.scope = *scope;
  observation.reporter = *observer;
  observation.generation = fr::ObservationGeneration(1);
  observation.evidence = fr::EvidenceClass::Synthetic;
  observation.complete = true;
  observation.authoritative_absence = true;

  struct Fixture {
    const char* subject;
    unsigned desired_mtu;
    unsigned observed_mtu;
    bool present;
  };
  const Fixture fixtures[] = {
      {"node/n1/port/eth0", 9000, 9000, true},   // already converged
      {"node/n2/port/eth0", 9000, 1500, true},   // mismatched
      {"node/n3/port/eth0", 9000, 0, false},     // missing from the fabric
  };
  for (const Fixture& fixture : fixtures) {
    const auto subject = fr::SubjectId::TryParse(fixture.subject);
    const auto key = fr::AttributeKey::TryParse("mtu");
    if (!subject.has_value() || !key.has_value()) {
      return Fail("synthetic fixture identities are invalid");
    }
    fr::SubjectIntent entry;
    entry.id = *subject;
    entry.desired.emplace(*key, fr::AttributeValue::Unsigned(fixture.desired_mtu));
    intent.subjects.emplace(*subject, std::move(entry));
    if (fixture.present) {
      fr::SubjectObservation seen;
      seen.id = *subject;
      seen.observed.emplace(*key, fr::AttributeValue::Unsigned(fixture.observed_mtu));
      observation.subjects.emplace(*subject, std::move(seen));
    }
  }
  const auto extra = fr::SubjectId::TryParse("node/n9/port/eth0");
  const auto extra_key = fr::AttributeKey::TryParse("mtu");
  if (extra.has_value() && extra_key.has_value()) {
    fr::SubjectObservation seen;
    seen.id = *extra;
    seen.observed.emplace(*extra_key, fr::AttributeValue::Unsigned(9000));
    observation.subjects.emplace(*extra, std::move(seen));
  }

  fr::IntentCommitResult intent_result;
  const fr::Status committed = engine->CommitIntent(intent, intent_result);
  if (!committed.ok()) {
    return Fail(committed.ToString());
  }
  fr::ObservationCommitResult observation_result;
  const fr::Status recorded =
      engine->RecordObservationAt(observation, static_cast<fr::UnixMillis>(options.now),
                                  observation_result);
  if (!recorded.ok()) {
    return Fail(recorded.ToString());
  }

  fr::PlanRequest request;
  request.scope = *scope;
  request.now_unix_ms = static_cast<fr::UnixMillis>(options.now);
  request.dry_run = true;
  const auto plan = engine->Plan(request);
  if (!plan.ok()) {
    return Fail(plan.status().ToString());
  }

  std::printf("%s", fr::RenderPlanText(*plan).c_str());
  std::printf("scenario plan-id=%s\n", plan->plan_id.c_str());
  if (!options.out.empty()) {
    const int written = WriteFile(options.out, fr::ToJson(*plan) + "\n");
    if (written != 0) {
      return Fail("could not write the scenario plan");
    }
  }

  // Determinism: the same evidence must reproduce the same plan identifier.
  const auto replay = engine->Plan(request);
  if (!replay.ok()) {
    return Fail(replay.status().ToString());
  }
  if (replay->plan_id != plan->plan_id) {
    return Fail("plan identity is not deterministic across identical requests");
  }
  std::printf("scenario deterministic=yes\n");
  return 0;
}

int CommandSelftest(const Options& options) {
  (void)options;
  const fr::Sha256Digest digest = fr::Sha256Of(std::string_view("abc"));
  const char* expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  if (fr::ToHex(digest) != expected) {
    return Fail("SHA-256 self test failed");
  }
  const fr::BootId first = fr::BootId::Generate();
  const fr::BootId second = fr::BootId::Generate();
  if (first == second) {
    return Fail("boot identities are not unique");
  }
  std::printf("selftest ok version=%s\n", fr::kVersionString);
  return 0;
}

void PrintUsage() {
  std::printf(
      "%s %s\n"
      "usage: fabric-reconciliation <command> [options]\n\n"
      "commands:\n"
      "  init      --store DIR [--quiet]\n"
      "  status    --store DIR [--json]\n"
      "  policy    --store DIR [--file POLICY.json]\n"
      "  intent    --store DIR --file INTENT.json\n"
      "  observe   --store DIR --file OBSERVATION.json [--now MS] [--json]\n"
      "  classify  --store DIR --scope SCOPE [--now MS] [--json]\n"
      "  plan      --store DIR --scope SCOPE [--now MS] [--dry-run] [--json] [--out FILE]\n"
      "  explain   --store DIR --scope SCOPE [--now MS]\n"
      "  dispatch  --store DIR --scope SCOPE [--now MS] [--plan-id ID]\n"
      "  complete  --store DIR --attempt N --key HEX --state STATE [--now MS]\n"
      "  verify    --store DIR --file VERIFICATION.json\n"
      "  fence     --store DIR --scope SCOPE --below EPOCH [--subject ID]\n"
      "  attempts  --store DIR [--scope SCOPE] [--json]\n"
      "  outcomes  --store DIR [--scope SCOPE]\n"
      "  serve     --store DIR [--address A] [--port N] [--ready-file F] [--stdin-control]\n"
      "  scenario  --store DIR [--now MS] [--out FILE]\n"
      "  selftest\n",
      fr::kProductName, fr::kVersionString);
}

std::string Next(const std::vector<std::string>& argv, std::size_t& index) {
  ++index;
  if (index >= argv.size()) {
    return std::string();
  }
  return argv[index];
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> raw;
  for (int index = 1; index < argc; ++index) {
    raw.emplace_back(argv[index]);
  }
  if (!raw.empty() && raw[0] == "--version") {
    std::printf("%s\n", fr::kVersionString);
    return 0;
  }
  if (raw.empty() || raw[0] == "--help" || raw[0] == "-h") {
    PrintUsage();
    return raw.empty() ? 2 : 0;
  }
  Options options;
  options.command = raw[0];
  for (std::size_t index = 1; index < raw.size(); ++index) {
    const std::string& token = raw[index];
    if (token == "--store") { options.store = Next(raw, index); }
    else if (token == "--scope") { options.scope = Next(raw, index); }
    else if (token == "--subject") { options.subject = Next(raw, index); }
    else if (token == "--file") { options.file = Next(raw, index); }
    else if (token == "--out") { options.out = Next(raw, index); }
    else if (token == "--address") { options.address = Next(raw, index); }
    else if (token == "--plan-id") { options.plan_id = Next(raw, index); }
    else if (token == "--state") { options.state = Next(raw, index); }
    else if (token == "--key") { options.key = Next(raw, index); }
    else if (token == "--ready-file") { options.ready_file = Next(raw, index); }
    else if (token == "--now") { options.now = std::strtoll(Next(raw, index).c_str(), nullptr, 10); }
    else if (token == "--port") { options.port = std::strtoll(Next(raw, index).c_str(), nullptr, 10); }
    else if (token == "--attempt") { options.attempt = std::strtoll(Next(raw, index).c_str(), nullptr, 10); }
    else if (token == "--below") { options.below = std::strtoll(Next(raw, index).c_str(), nullptr, 10); }
    else if (token == "--json") { options.json = true; }
    else if (token == "--dry-run") { options.dry_run = true; }
    else if (token == "--quiet") { options.quiet = true; }
    else if (token == "--stdin-control") { options.stdin_control = true; }
    else if (token == "--version") { std::printf("%s\n", fr::kVersionString); return 0; }
    else if (token == "--help" || token == "-h") { PrintUsage(); return 0; }
    else { return Fail("unknown argument: " + token); }
  }

  if (options.command == "init") { return CommandInit(options); }
  if (options.command == "status") { return CommandStatus(options); }
  if (options.command == "policy") { return CommandPolicy(options); }
  if (options.command == "intent") { return CommandIntent(options); }
  if (options.command == "observe") { return CommandObserve(options); }
  if (options.command == "classify") { return CommandClassify(options); }
  if (options.command == "plan") { return CommandPlan(options, false); }
  if (options.command == "explain") { return CommandPlan(options, true); }
  if (options.command == "dispatch") { return CommandDispatch(options); }
  if (options.command == "complete") { return CommandComplete(options); }
  if (options.command == "verify") { return CommandVerify(options); }
  if (options.command == "fence") { return CommandFence(options); }
  if (options.command == "attempts") { return CommandAttempts(options); }
  if (options.command == "outcomes") { return CommandOutcomes(options); }
  if (options.command == "serve") { return CommandServe(options); }
  if (options.command == "scenario") { return CommandScenario(options); }
  if (options.command == "selftest") { return CommandSelftest(options); }
  PrintUsage();
  return Fail("unknown command: " + options.command);
}
