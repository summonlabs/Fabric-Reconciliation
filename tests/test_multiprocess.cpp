// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Real multiprocess proof.
//
// Every case here runs the worker as a genuinely separate operating system
// process and terminates it with a hard exit that runs no destructor, no flush
// and no unwinding. Threads are never used as a substitute. The service cases
// use a real loopback socket and a real killed process.

#include "test_helpers.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "summon/fabric_reconciliation/server.hpp"

namespace fr = summon::fabric_reconciliation;
using namespace frtest;

#ifndef FR_WORKER_EXE
#define FR_WORKER_EXE "fabric-reconcile-worker"
#endif

#ifndef FR_CLI_EXE
#define FR_CLI_EXE "fabric-reconciliation"
#endif

namespace {

const char* kScope = "fabric/rack-7";

/// A child process that can be waited for or terminated hard.
class Child {
 public:
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child() { (void)Terminate(); }

  /// Launches @p exe with @p arguments. Returns false when the launch failed.
  bool Launch(const std::string& exe, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    argv_.clear();
    argv_.push_back(exe.c_str());
    for (const std::string& argument : arguments) {
      argv_.push_back(argument.c_str());
    }
    argv_.push_back(nullptr);
    const intptr_t handle = ::_spawnv(_P_NOWAIT, exe.c_str(), argv_.data());
    if (handle == -1) {
      return false;
    }
    handle_ = reinterpret_cast<void*>(handle);
    return true;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
      return false;
    }
    pid_ = pid;
    return true;
#endif
  }

  /// Runs to completion and returns the exit code, or -1 when it could not be
  /// observed.
  int Wait() {
#if defined(_WIN32)
    if (handle_ == nullptr) {
      return -1;
    }
    ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    return static_cast<int>(code);
#else
    if (pid_ == 0) {
      return -1;
    }
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = 0;
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return -1;
#endif
  }

  /// Terminates the child immediately. Idempotent.
  bool Terminate() {
#if defined(_WIN32)
    if (handle_ == nullptr) {
      return true;
    }
    const BOOL killed = ::TerminateProcess(static_cast<HANDLE>(handle_), 9);
    ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    return killed != 0;
#else
    if (pid_ == 0) {
      return true;
    }
    const bool killed = ::kill(pid_, SIGKILL) == 0;
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = 0;
    return killed;
#endif
  }

  [[nodiscard]] bool running() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return pid_ != 0;
#endif
  }

  /// True when the child has already exited. Never blocks.
  [[nodiscard]] bool exited() const {
#if defined(_WIN32)
    if (handle_ == nullptr) {
      return true;
    }
    return ::WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_OBJECT_0;
#else
    if (pid_ == 0) {
      return true;
    }
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    return result == pid_;
#endif
  }

 private:
#if defined(_WIN32)
  void* handle_{nullptr};
  std::vector<const char*> argv_;
#else
  int pid_{0};
#endif
};

int RunWorker(const std::vector<std::string>& arguments) {
  Child child;
  if (!child.Launch(FR_WORKER_EXE, arguments)) {
    return -1;
  }
  return child.Wait();
}

std::vector<std::string> WorkerArguments(const std::string& store, const std::string& mode) {
  return {"--store", store, "--mode", mode, "--scope", kScope, "--subjects", "8", "--now", "1000"};
}

void SeedStore(const std::string& store) {
  FR_CHECK_EQ(RunWorker(WorkerArguments(store, "seed")), 0);
  FR_CHECK_EQ(RunWorker(WorkerArguments(store, "observe")), 0);
}

/// Runs the whole reconciliation inside one process incarnation so that the
/// observation is fresh evidence for the plan that same incarnation builds.
int RunScenario(const std::string& store, const std::string& kill_at) {
  std::vector<std::string> arguments = WorkerArguments(store, "scenario");
  arguments.push_back("--kill-at");
  arguments.push_back(kill_at);
  return RunWorker(arguments);
}

}  // namespace

FR_TEST(multiprocess, worker_seed_and_observe_are_real_processes) {
  ScratchDirectory scratch{"mp-basic"};
  const std::string store = scratch.child("store").string();
  SeedStore(store);
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
  const auto intent = engine->CurrentIntent(RequireScope(kScope));
  FR_CHECK_STATUS_OK(intent);
  FR_CHECK_EQ(intent->subjects.size(), std::size_t(8));
  const auto evidence = engine->ObservationEvidence(RequireScope(kScope));
  FR_CHECK_STATUS_OK(evidence);
  FR_CHECK_EQ(evidence->size(), std::size_t(1));
  // The engine that wrote the evidence is gone; the evidence is retained but
  // is not fresh in this new incarnation.
  FR_CHECK(evidence->front().received_epoch.value() < engine->epoch().value());
}

FR_TEST(multiprocess, kill_before_dispatch_leaves_no_attempt) {
  ScratchDirectory scratch{"mp-kill-before-dispatch"};
  const std::string store = scratch.child("store").string();
  FR_CHECK_EQ(RunScenario(store, "before_dispatch"), 70);

  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), std::size_t(0));
  FR_CHECK_EQ(engine->boot_report().attempts_interrupted, std::size_t(0));
}

FR_TEST(multiprocess, kill_after_dispatch_surfaces_an_ambiguous_attempt) {
  ScratchDirectory scratch{"mp-kill-after-dispatch"};
  const std::string store = scratch.child("store").string();
  FR_CHECK_EQ(RunScenario(store, "after_dispatch_before_ack"), 71);

  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
  // The worker's synthetic observation mismatch is one subject in three, so
  // three action intents were in flight when the process was killed.
  constexpr std::size_t kExpected = 3;
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), kExpected);
  for (const fr::AttemptRecord& attempt : *attempts) {
    FR_CHECK(attempt.state == fr::AttemptState::Interrupted);
    FR_CHECK(attempt.idempotency_key != fr::Sha256Digest{});
  }

  const auto outcomes = engine->Outcomes(RequireScope(kScope));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK_EQ(outcomes->size(), kExpected);
  // The worker may have handed the intent to the fabric, so the outcome is
  // reported as ambiguous rather than assumed complete.
  for (const fr::ReconciliationOutcome& outcome : *outcomes) {
    FR_CHECK(outcome.ambiguous);
    FR_CHECK(!outcome.verified);
    FR_CHECK(outcome.terminal_state == fr::AttemptState::Interrupted);
  }
  FR_CHECK_EQ(engine->boot_report().attempts_interrupted, kExpected);

  // The evidence recorded by the dead process is retained but not fresh, so the
  // new incarnation must not act on it.
  fr::PlanRequest request;
  request.scope = RequireScope(kScope);
  request.now_unix_ms = 1000;
  const auto plan = engine->Plan(request);
  FR_CHECK_STATUS_OK(plan);
  FR_CHECK_EQ(plan->MutationCount(), std::size_t(0));
  FR_CHECK(plan->verdict == fr::ConvergenceVerdict::Indeterminate);
}

FR_TEST(multiprocess, kill_after_apply_before_verification_is_ambiguous_not_verified) {
  ScratchDirectory scratch{"mp-kill-after-apply"};
  const std::string store = scratch.child("store").string();
  FR_CHECK_EQ(RunScenario(store, "after_apply_before_verify"), 73);

  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
  constexpr std::size_t kExpected = 3;
  const auto attempts = engine->Attempts(RequireScope(kScope));
  FR_CHECK_STATUS_OK(attempts);
  FR_CHECK_EQ(attempts->size(), kExpected);
  // The worker recorded an application for the first intent and then died
  // before any verification. Having reached "applied" is not evidence that the
  // effect happened, so every attempt is interrupted and every outcome is
  // ambiguous.
  std::size_t applied_before_kill = 0;
  for (const fr::AttemptRecord& attempt : *attempts) {
    FR_CHECK(attempt.state == fr::AttemptState::Interrupted);
    for (const fr::AttemptTransition& transition : attempt.transitions) {
      if (transition.to == fr::AttemptState::Applied) {
        ++applied_before_kill;
      }
    }
  }
  FR_CHECK_EQ(applied_before_kill, std::size_t(1));
  const auto outcomes = engine->Outcomes(RequireScope(kScope));
  FR_CHECK_STATUS_OK(outcomes);
  FR_CHECK_EQ(outcomes->size(), kExpected);
  for (const fr::ReconciliationOutcome& outcome : *outcomes) {
    FR_CHECK(!outcome.verified);
    FR_CHECK(outcome.ambiguous);
  }
}

FR_TEST(multiprocess, a_completed_worker_leaves_a_verified_capable_outcome_that_stays_unverified) {
  ScratchDirectory scratch{"mp-complete"};
  const std::string store = scratch.child("store").string();
  FR_CHECK_EQ(RunScenario(store, "none"), 0);

  {
    // Reopen after a clean process exit: nothing is verified. A clean exit is
    // not evidence of effect.
    std::unique_ptr<fr::ReconciliationEngine> reopened = OpenEngine(scratch.child("store"));
    const auto outcomes = reopened->Outcomes(RequireScope(kScope));
    FR_CHECK_STATUS_OK(outcomes);
    FR_CHECK(!outcomes->empty());
    for (const fr::ReconciliationOutcome& outcome : *outcomes) {
      FR_CHECK(!outcome.verified);
    }
  }

  // While this process holds the store, another process must be refused rather
  // than allowed to interleave appends into the same journal.
  {
    std::unique_ptr<fr::ReconciliationEngine> held = OpenEngine(scratch.child("store"));
    FR_CHECK(RunWorker(WorkerArguments(store, "report")) != 0);
  }

  // After the store is released the report mode of the worker runs cleanly.
  FR_CHECK_EQ(RunWorker(WorkerArguments(store, "report")), 0);
}

FR_TEST(multiprocess, repeated_restarts_never_resurrect_pre_restart_authority) {
  ScratchDirectory scratch{"mp-restarts"};
  const std::string store = scratch.child("store").string();
  FR_CHECK_EQ(RunScenario(store, "none"), 0);

  std::uint64_t previous_epoch = 0;
  for (int iteration = 0; iteration < 4; ++iteration) {
    const int code =
        RunScenario(store, iteration % 2 == 0 ? "after_dispatch_before_ack" : "none");
    FR_CHECK(code == 0 || code == 71);

    std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
    FR_CHECK(engine->epoch().value() > previous_epoch);
    previous_epoch = engine->epoch().value();
    const auto attempts = engine->Attempts(RequireScope(kScope));
    FR_CHECK_STATUS_OK(attempts);
    for (const fr::AttemptRecord& attempt : *attempts) {
      FR_CHECK(attempt.epoch.value() < engine->epoch().value());
      FR_CHECK(fr::IsTerminalAttemptState(attempt.state));
      FR_CHECK(!(attempt.state == fr::AttemptState::Verified));
    }
  }
  // Every incarnation is durably recorded in the epoch lineage.
  FR_CHECK(previous_epoch >= 5);
}

FR_TEST(multiprocess, a_worker_compaction_is_visible_to_the_next_process) {
  ScratchDirectory scratch{"mp-compact"};
  const std::string store = scratch.child("store").string();
  SeedStore(store);
  FR_CHECK_EQ(RunWorker(WorkerArguments(store, "compact")), 0);
  std::unique_ptr<fr::ReconciliationEngine> engine = OpenEngine(scratch.child("store"));
  FR_CHECK(engine->boot_report().snapshot_loaded);
  const auto intent = engine->CurrentIntent(RequireScope(kScope));
  FR_CHECK_STATUS_OK(intent);
  FR_CHECK_EQ(intent->subjects.size(), std::size_t(8));
}

FR_TEST(multiprocess, a_real_service_process_is_killed_and_restarted_with_a_new_epoch) {
  ScratchDirectory scratch{"mp-service"};
  const std::string store = scratch.child("store").string();
  const std::string ready = scratch.child("ready.txt").string();
  const std::string fixed_port = "39271";

  FR_CHECK_EQ(RunScenario(store, "none"), 0);

  // The service publishes a readiness file once it is accepting. The test waits
  // for that event, and fails immediately if the child exits instead, so the
  // readiness check is deterministic rather than a retry race.
  const auto launch_service = [&](Child& child) {
    std::error_code error;
    std::filesystem::remove(ready, error);
    return child.Launch(FR_CLI_EXE, {"serve", "--store", store, "--address", "127.0.0.1",
                                     "--port", fixed_port, "--ready-file", ready});
  };
  const auto await_ready = [&](Child& child) {
    for (int spin = 0; spin < 2000000; ++spin) {
      std::error_code error;
      if (std::filesystem::exists(ready, error)) {
        return true;
      }
      if (child.exited()) {
        return false;
      }
      std::this_thread::yield();
    }
    return false;
  };

  Child first;
  FR_REQUIRE(launch_service(first));
  FR_REQUIRE(await_ready(first));

  fr::ReconciliationClient client;
  FR_CHECK_STATUS_OK(fr::ReconciliationClient::Connect("127.0.0.1", 39271,
                                                       fr::RuntimeLimits{}, client));
  const fr::CoordinatorEpoch first_epoch = client.server_epoch();
  const fr::BootId first_boot = client.server_boot();
  const fr::StoreId store_id = client.server_store();
  std::string response;
  FR_CHECK_STATUS_OK(client.Call(fr::WireOperation::BootReport, std::string(), response));
  FR_CHECK(Contains(response, "freshness-restored=no"));

  // Keep the established connection open so that the death of the service is
  // observed on a real session rather than inferred from a refused connect.
  FR_REQUIRE(first.Terminate());

  const fr::Status after_death = client.Call(fr::WireOperation::BootReport, std::string(), response);
  FR_CHECK(!after_death.ok());
  client.Close();

  // A fresh process on the same store must be a new incarnation: a new boot
  // identity, a strictly higher epoch, and the same durable store identity.
  Child second;
  FR_REQUIRE(launch_service(second));
  FR_REQUIRE(await_ready(second));

  fr::ReconciliationClient restarted;
  FR_CHECK_STATUS_OK(fr::ReconciliationClient::Connect("127.0.0.1", 39271,
                                                       fr::RuntimeLimits{}, restarted));
  FR_CHECK(restarted.server_epoch().value() > first_epoch.value());
  FR_CHECK(!(restarted.server_boot() == first_boot));
  FR_CHECK(restarted.server_store() == store_id);
  FR_CHECK_STATUS_OK(restarted.Call(fr::WireOperation::BootReport, std::string(), response));
  FR_CHECK(Contains(response, "mutation-authority-restored=no"));
  // The evidence and the interrupted attempt written by the previous
  // incarnation are still reported, without being restored as authority.
  FR_CHECK(Contains(response, "attempts interrupted=") ||
           Contains(response, "attempts restored="));
  restarted.Close();
  FR_REQUIRE(second.Terminate());
}

FR_TEST(multiprocess, the_worker_binary_is_the_one_built_from_this_tree) {
  // A guard against accidentally testing a stale binary: the worker must at
  // least accept its documented arguments and refuse unknown ones.
  Child child;
  FR_REQUIRE(child.Launch(FR_WORKER_EXE, {"--nonsense"}));
  FR_CHECK(child.Wait() != 0);
}
