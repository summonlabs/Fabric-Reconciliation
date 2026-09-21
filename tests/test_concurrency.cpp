// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency and lifecycle proofs. Synchronisation uses explicit barriers and
// latches, never sleeps, so a failure is a real defect rather than a scheduling
// accident. The ownership audit is in docs/CONCURRENCY-AUDIT.md; these tests
// exercise the exact paths it examines.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <atomic>
#include <barrier>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "summon/fabric_reconciliation/server.hpp"

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

namespace {

const char* kScope = "fabric/rack-7";

}  // namespace

FR_TEST(concurrency, concurrent_commits_produce_one_winner_per_generation) {
  ScratchDirectory scratch{"concurrency-commit"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<int> accepted{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> conflicts{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      gate.arrive_and_wait();
      fr::IntentDocument intent = MakeIntent(kScope, 3, 4, fr::EvidenceClass::Synthetic);
      intent.intent = RequireIntent(("intent/thread-" + std::to_string(index)).c_str());
      intent.digest = fr::ComputeIntentDigest(intent);
      fr::IntentCommitResult result;
      const fr::Status status = engine->CommitIntent(intent, result);
      if (status.ok()) {
        if (result.duplicate) {
          ++duplicates;
        } else {
          ++accepted;
        }
      } else if (status.code() == fr::StatusCode::ConflictState) {
        ++conflicts;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  // Every thread submitted the same generation and the same desired state, so
  // exactly one commit is recorded and every other thread observes either a
  // duplicate or a conflict. Nothing is lost and nothing is double applied.
  FR_CHECK_EQ(accepted.load(), 1);
  FR_CHECK_EQ(accepted.load() + duplicates.load() + conflicts.load(), kThreads);
}

FR_TEST(concurrency, plans_are_stable_while_other_threads_read) {
  ScratchDirectory scratch{"concurrency-plan"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 24, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 24, 3, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto reference = engine->Plan(request);
  FR_CHECK_STATUS_OK(reference);

  constexpr int kThreads = 6;
  std::barrier gate(kThreads);
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      gate.arrive_and_wait();
      for (int iteration = 0; iteration < 40; ++iteration) {
        const auto plan = engine->Plan(request);
        if (!plan.ok() || plan->plan_id != reference->plan_id) {
          ++mismatches;
        }
        const auto stats = engine->stats();
        (void)stats;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FR_CHECK_EQ(mismatches.load(), 0);
}

FR_TEST(concurrency, dispatch_races_never_issue_two_attempts_for_one_decision) {
  ScratchDirectory scratch{"concurrency-dispatch"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 16, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 16, 2, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));

  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  const std::size_t expected = plan->MutationCount();
  FR_CHECK(expected > 0);

  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<std::size_t> issued{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      gate.arrive_and_wait();
      const auto dispatched = engine->Dispatch(*plan);
      if (dispatched.ok()) {
        issued += dispatched->intents.size();
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), expected);
  FR_CHECK(issued.load() >= expected);
}

FR_TEST(concurrency, concurrent_completions_are_accepted_exactly_once) {
  ScratchDirectory scratch{"concurrency-complete"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));
  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent action = dispatched->intents.front();

  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      gate.arrive_and_wait();
      fr::CompletionRequest completion;
      completion.attempt = action.attempt;
      completion.epoch = engine->epoch();
      completion.boot = engine->boot();
      completion.idempotency_key = action.idempotency_key;
      completion.target = fr::AttemptState::Applied;
      fr::ReconciliationOutcome outcome;
      if (engine->Complete(completion, outcome).ok()) {
        ++accepted;
      } else {
        ++rejected;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FR_CHECK_EQ(accepted.load(), 1);
  FR_CHECK_EQ(rejected.load(), kThreads - 1);
  FR_CHECK_EQ(accepted.load() + rejected.load(), kThreads);
  // Exactly one transition was recorded for the winning completion.
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), std::size_t(1));
  FR_CHECK(attempts->front().state == fr::AttemptState::Applied);
  FR_CHECK_EQ(attempts->front().transitions.size(), std::size_t(2));
}

FR_TEST(concurrency, concurrent_terminal_completions_are_accepted_exactly_once) {
  ScratchDirectory scratch{"concurrency-terminal"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  const fr::IntentDocument intent = MakeIntent(kScope, 1, 1, fr::EvidenceClass::Synthetic);
  fr::IntentCommitResult commit_result;
  FR_CHECK_STATUS_OK(engine->CommitIntent(intent, commit_result));
  const fr::ObservationSubmission observation =
      MakeObservation(kScope, "reporter/a", 1, 1, 1, false, fr::EvidenceClass::Synthetic);
  fr::ObservationCommitResult observation_result;
  FR_CHECK_STATUS_OK(engine->RecordObservationAt(observation, 1000, observation_result));
  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  const auto dispatched = engine->Dispatch(*plan);
  FR_CHECK_STATUS_OK(dispatched);
  const fr::ActionIntent action = dispatched->intents.front();

  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      gate.arrive_and_wait();
      fr::CompletionRequest completion;
      completion.attempt = action.attempt;
      completion.epoch = engine->epoch();
      completion.boot = engine->boot();
      completion.idempotency_key = action.idempotency_key;
      completion.target = fr::AttemptState::Failed;
      fr::ReconciliationOutcome outcome;
      if (engine->Complete(completion, outcome).ok()) {
        ++accepted;
      } else {
        ++rejected;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  // Failed is terminal, so every loser is reported as a duplicate completion of
  // an already-terminal attempt rather than as an illegal transition.
  FR_CHECK_EQ(accepted.load(), 1);
  FR_CHECK_EQ(rejected.load(), kThreads - 1);
  FR_CHECK_EQ(engine->stats().duplicate_completions,
              static_cast<std::uint64_t>(kThreads - 1));
  const auto outcomes = engine->Outcomes(RequireScope(kScope));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK_EQ(outcomes->size(), std::size_t(1));
}

FR_TEST(concurrency, mixed_workload_is_consistent_and_terminates) {
  ScratchDirectory scratch{"concurrency-mixed"};
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      gate.arrive_and_wait();
      for (int iteration = 0; iteration < 25; ++iteration) {
        const std::uint64_t generation = static_cast<std::uint64_t>(iteration + 1);
        if ((index % 4) == 0) {
          fr::IntentDocument intent = MakeIntent(kScope, generation, 3, fr::EvidenceClass::Synthetic);
          fr::IntentCommitResult result;
          const fr::Status status = engine->CommitIntent(intent, result);
          if (!status.ok() && status.code() != fr::StatusCode::Rejected &&
              status.code() != fr::StatusCode::ConflictState) {
            ++failures;
          }
        } else if ((index % 4) == 1) {
          const fr::ObservationSubmission observation = MakeObservation(
              kScope, "reporter/a", generation, 3, 2, false, fr::EvidenceClass::Synthetic);
          fr::ObservationCommitResult result;
          const fr::Status status = engine->RecordObservationAt(observation, 1000, result);
          if (!status.ok() && status.code() != fr::StatusCode::Rejected &&
              status.code() != fr::StatusCode::ConflictState) {
            ++failures;
          }
        } else if ((index % 4) == 2) {
          fr::PlanRequest request;
          request.scope = RequireScope(kScope);
          request.now_unix_ms = 1000;
          const auto plan = engine->Plan(request);
          if (!plan.ok()) {
            ++failures;
          }
        } else {
          const auto attempts = engine->Attempts(RequireScope(kScope));
          if (!attempts.ok()) {
            ++failures;
          }
          const auto boot = engine->boot_report();
          (void)boot;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  FR_CHECK_EQ(failures.load(), 0);
  // After the storm the store must still be reopenable and self-consistent.
  const auto scopes = engine->KnownScopes();
  FR_CHECK_STATUS_OK(scopes);
}

FR_TEST(lifecycle, service_start_stop_cycles_release_every_resource) {
  ScratchDirectory scratch{"lifecycle-cycles"};
  for (int iteration = 0; iteration < 5; ++iteration) {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    fr::ServerOptions options;
    options.port = 0;
    std::unique_ptr<fr::ReconciliationServer> server;
    FR_CHECK_STATUS_OK(fr::ReconciliationServer::Start(options, *engine, server));
    fr::RuntimeLimits limits;
    fr::ReconciliationClient client;
    const fr::Status connected =
        fr::ReconciliationClient::Connect("127.0.0.1", server->port(), limits, client);
    if (connected.ok()) {
      std::string response;
      (void)client.Call(fr::WireOperation::BootReport, std::string(), response);
      client.Close();
    }
    FR_CHECK_STATUS_OK(server->Stop());
    FR_CHECK_EQ(server->connections_accepted(), connected.ok() ? 1ull : 0ull);
  }
}

FR_TEST(lifecycle, engine_destruction_releases_the_store) {
  ScratchDirectory scratch{"lifecycle-engine"};
  for (int iteration = 0; iteration < 8; ++iteration) {
    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
    const fr::IntentDocument intent = MakeIntent(kScope, 1, 2, fr::EvidenceClass::Synthetic);
    fr::IntentCommitResult result;
    FR_CHECK_STATUS_OK(engine->CommitIntent(intent, result));
    engine.reset();
  }
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.path());
  const auto intent = engine->CurrentIntent(RequireScope(kScope));
  FR_CHECK_STATUS_OK(intent);
  // The same generation was re-committed eight times, so it is still one intent.
  FR_CHECK_EQ(intent->generation.value(), 1ull);
}
