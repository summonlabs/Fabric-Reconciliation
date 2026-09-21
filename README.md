# Fabric Reconciliation

Fabric Reconciliation is a Summon Software Labs runtime for one question:

> Given intended authoritative fabric state and the current observed state after
> a disruption, **what differs, which differences are trustworthy, what
> reconciliation actions are legal right now, and when must convergence pause,
> conflict, fence, or require revalidation?**

Version 1.0.0. Portable C++20, CMake, no third-party runtime dependency.

## Systems boundary

The runtime owns **post-disruption comparison and reconciliation intent**. It
answers the question above by producing a deterministic, generation-bound,
authority-checked plan, and by persisting the lineage that made each decision
legal.

It owns:

* comparison of intended state against observed state, per scope and subject;
* deterministic classification of every difference;
* eligibility and authority decisions about which differences may become
  reconciliation actions;
* idempotent, generation-bound action intents and their lifecycle;
* verification of applied effects against independent evidence;
* fences, epochs and restart semantics for all of the above;
* dry-run and explain surfaces;
* durable lineage: definitions, policy, committed intents, recorded evidence,
  attempts, outcomes and fences.

It does **not** own, and does not pretend to own:

* topology discovery;
* execution of route, switch, NIC or queue mutations;
* telemetry collection;
* deciding general recovery strategy when several recovery modes exist;
* authentication, authorisation policy or secure transport.

Integration with adjacent runtimes happens through explicit typed inputs and
outputs: intent documents, observation submissions, policy documents, action
intents, verification evidence and committed outcomes. Nothing else crosses the
boundary.

## Authority model

Seven quantities are kept distinct in the type system, in durable storage and
in every decision:

| Quantity | Meaning | Is it authority? |
| --- | --- | --- |
| desired | what the committed intent says should be true | authority for *what should be true*, never proof of application |
| observed | what a reporter says it saw | evidence only, never authority |
| acknowledged | a target said it received the request | no |
| applied | a target said it executed the request | no |
| verified | independent evidence confirms the effect | the strongest claim the runtime makes, and only against evidence it holds |
| current | the obligation-free "what is live right now" for a scope | a property of the live process, not of a record |
| eligible | policy permits the action | no; eligibility is not authorisation |

Every externally visible decision carries an **authority vector** naming the
exact bindings that made it legal:

```
coordinator epoch, process boot identity,
policy identity + version + digest,
definition identity,
intent identity + generation + digest,
observation identity + generation + reporter + digest + receipt epoch + receipt time
```

Authority is proven only when every one of those bindings matches live
authority, the evidence is fresh, and no fence covers the subject. Otherwise the
decision is `FENCED` or `BLOCKED` with a deterministic reason and a
component mask naming exactly which binding failed. Fail-closed is the default:
an unknown or unset binding never counts as a match.

Revocation is automatic. Any change to the committed intent, the active policy,
the current observation evidence, the coordinator epoch or the fence table makes
an outstanding plan undispatchable, and a pre-restart completion unacceptable.

## Drift vocabulary

Nothing is folded into success or into ordinary absence.

| Class | Meaning |
| --- | --- |
| `ALREADY_CONVERGED` | intent and fresh observation agree on every managed attribute |
| `MISSING` | the intent expects the subject; a fresh observation with complete coverage and the right to assert absence says it is not there |
| `UNEXPECTED` | a fresh observation reports a subject a complete intent does not declare |
| `MISMATCHED` | both exist and at least one managed attribute differs |
| `STALE_OBSERVATION` | the best evidence for the scope is not fresh; the runtime does not fall back to an older generation |
| `STALE_INTENT` | the intent is behind the committed lineage for the scope |
| `CONFLICT` | two publishers disagree at one generation |
| `UNKNOWN` | coverage is insufficient to decide. Explicitly not "already satisfied" |
| `UNSUPPORTED` | outside the supported comparison class |

Two rules carry most of the weight:

* **Absence must be proven.** `MISSING` requires an observation that declares
  complete coverage *and* the right to assert absence. Without both, the subject
  is `UNKNOWN`.
* **A coverage gap is never an absence.** An attribute the observation did not
  cover is `UNKNOWN`, never `MISMATCHED` and never `ALREADY_CONVERGED`.

Refresh policy is fail-closed by default: `require_current_epoch` is true, so
evidence recorded by a previous process incarnation is stale no matter how
recent its timestamp is.

## Plan verdicts

| Verdict | Meaning |
| --- | --- |
| `CONVERGED_PROVEN` | every subject in scope was decided with complete coverage and no difference remains |
| `CONVERGENCE_REQUIRED` | legal, authorised actions exist |
| `PROVEN_BLOCKED` | no legal plan reaches the intent under the current policy and authority; the certificate is the complete list of blocking decisions |
| `INDETERMINATE` | coverage gaps, staleness, conflicts or unsupported subjects prevent a decision. Explicitly not success |
| `SEARCH_LIMIT_REACHED` | a configured bound stopped classification. Proof strength is reduced and the plan says so |

## Product-defining invariants

1. **Convergence emits nothing.** A plan over an already-converged scope contains
   zero mutation decisions and dispatching it issues zero attempts.
2. **Plans are pure functions of their evidence.** Two stores with the same
   committed intent and the same observation produce byte-identical plans,
   including the plan identifier. Plans do not depend on container, hash,
   insertion or discovery order.
3. **Actions are idempotent and generation-bound.** The idempotency key binds
   scope, subject, action, intent generation and digest, observation generation
   and digest, the desired state and the expected observed state. Re-dispatching
   the same plan reuses the existing attempt instead of issuing a second one.
4. **No action without proven authority.** Every actionable decision is
   re-checked against live authority; if anything changed, the whole plan is
   refused.
5. **Nothing newer is overwritten by something older.** A regressed intent or
   observation generation is refused; a divergent publication at one generation
   is a conflict.
6. **Acknowledgement is not application, and application is not verified
   effect.** Verification requires independent evidence the runtime actually
   holds, strictly newer than the observation the action was based on.
7. **Persistence is not liveness.** A restart never restores telemetry
   freshness, leases, in-flight mutation authority or backend effects.
8. **Ambiguity is surfaced, not resolved.** An attempt that was in flight when a
   process died is reported as `INTERRUPTED` and its outcome as `ambiguous`,
   never as complete.
9. **Fail-closed.** UNKNOWN, STALE, CONFLICT, INVALID and UNSUPPORTED are
   first-class outcomes and are never mapped onto success.
10. **Bounded.** Every table, history, queue, frame, document and explanation is
    bounded, and exceeding a bound is a deterministic refusal.

## Lifecycle and restart semantics

On every start the runtime:

1. takes an exclusive operating-system lock on the store directory, so two live
   writers can never interleave records into one journal;
2. removes a staging file left behind by an interrupted snapshot publication;
3. loads the snapshot, if any, and replays the journal above its watermark;
4. advances the coordinator epoch durably **before** serving anything;
5. generates a fresh process boot identity;
6. fences every attempt that was in flight:
   * an attempt that never left the process is `ABANDONED` and its outcome is
     not ambiguous, because it cannot have produced an effect;
   * an attempt at or beyond `DISPATCHED` is `INTERRUPTED` and its outcome is
     `ambiguous`, because the runtime refuses to assume the effect did not
     happen;
7. reports all of this in a boot report that states explicitly that freshness
   and mutation authority were **not** restored.

What survives a restart: policy, committed intents and their generation lineage,
recorded observation evidence (as evidence, never as freshness), attempt
lineage, committed outcomes and fences.

What does not survive, and is never reconstructed from disk: telemetry
freshness, leases, grants, in-flight authority, and backend effects implied by
an attempt record.

## Durable formats

`<store>/store.journal` is an append-only log:

* a fixed 96-byte header with magic, format version, header size, store identity,
  creation time, the sequence the file continues from, and a SHA-256 digest over
  the header;
* 56-byte record headers with magic, version, type, flags, payload length,
  sequence and a SHA-256 digest over the header prefix and the payload;
* contiguous sequence numbers; a regression or a gap is a hard integrity failure;
* a genuine torn tail — a final record that runs past the end of the file — is
  discarded and **reported**, and only the incomplete suffix is discarded;
* a *complete* record that fails its digest is never repaired and never
  truncated: it is reported as an integrity failure.

Writes are append, flush, fsync, and success is reported only after the barrier.

`<store>/store.snapshot` is a checkpoint written to a staging file, flushed, and
atomically renamed into place. It carries the journal sequence watermark it
incorporates, so replay starts exactly after it. Compaction publishes the
snapshot durably and then starts a fresh journal that continues the sequence, so
record numbering is globally monotonic across every journal generation of one
store.

`<store>/store.lock` carries the exclusive ownership lock. It is an operating
system lock, not a marker file: the operating system releases it when the
process exits for any reason, including a hard kill, so no stale-lock heuristic
exists and none can be wrong.

## Transport

The runtime can expose the engine over a bounded framed protocol on a loopback
socket.

Frame layout (64-byte header):

```
[0,4)   magic 'F','R','P','1'
[4,6)   protocol version
[6,8)   message type
[8,10)  flags
[10,12) reserved, must be zero
[12,16) payload length
[16,24) session id
[24,32) sequence
[32,64) SHA-256 over bytes [0,32) and the payload
```

* declared payload lengths are checked against the configured maximum before any
  allocation proportional to them;
* decoding is total and its failures are sticky: once a reader has rejected a
  frame it refuses every later frame on that stream;
* every enum, domain and range is validated;
* truncated frames, corrupt digests, regressed sequences and trailing bytes are
  refused;
* each connection establishes a session carrying the client boot identity, the
  coordinator epoch and the store identity observed at handshake; every request
  is checked against that session, so one session can never act under another
  session's identity, boot or epoch;
* shutdown releases blocked accepts and blocked reads deterministically through
  a wake channel that the waiting thread selects on together with its socket.
  No timeout, watchdog or polling is involved.

**Trust boundary.** The transport provides integrity against corruption and
against cross-session confusion. It does **not** provide authentication,
confidentiality or protection against an active on-path attacker, and the
runtime does not claim that it does. SHA-256 appears here and in the durable
formats purely as a corruption and torn-write detector.

## Build, test, install

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /some/prefix
```

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `FABRIC_RECONCILIATION_BUILD_TOOLS` | ON | command line tools |
| `FABRIC_RECONCILIATION_BUILD_TESTS` | ON | test suites |
| `FABRIC_RECONCILIATION_BUILD_EXAMPLES` | ON | the example program |
| `FABRIC_RECONCILIATION_BUILD_BENCH` | ON | the scale benchmark |
| `FABRIC_RECONCILIATION_WARNINGS_AS_ERRORS` | ON | `/WX` or `-Werror` |
| `FABRIC_RECONCILIATION_ENABLE_SANITIZERS` | OFF | AddressSanitizer |

Requirements: a C++20 compiler, CMake 3.20 or newer, and threads. On Windows the
operating system sockets library is used; nothing else is required.

### Downstream use

```cmake
find_package(FabricReconciliation CONFIG REQUIRED)
target_link_libraries(app PRIVATE SummonLabs::FabricReconciliation::fabric_reconciliation)
```

A complete downstream program lives in `tests/consumer`. It is deliberately
built outside the source tree against an install prefix, uses only installed
headers and the exported target, and fails if any product-defining invariant does
not hold.

```cpp
#include <summon/fabric_reconciliation/engine.hpp>

summon::fabric_reconciliation::EngineOptions options;
options.store_dir = "fabric-store";
std::unique_ptr<summon::fabric_reconciliation::ReconciliationEngine> engine;
summon::fabric_reconciliation::ReconciliationEngine::Open(options, engine);

// Commit intended state, record observed evidence, then:
auto plan = engine->Plan(request);        // deterministic, generation-bound
auto issued = engine->Dispatch(*plan);    // idempotent, authority-checked
```

## Tools

`fabric-reconciliation` is the operator surface. Every command works on a real
durable store through the same public API the library exposes.

```
fabric-reconciliation init      --store DIR [--quiet]
fabric-reconciliation status    --store DIR [--json]
fabric-reconciliation policy    --store DIR [--file POLICY.json]
fabric-reconciliation intent    --store DIR --file INTENT.json
fabric-reconciliation observe   --store DIR --file OBSERVATION.json [--now MS] [--json]
fabric-reconciliation classify  --store DIR --scope SCOPE [--now MS] [--json]
fabric-reconciliation plan      --store DIR --scope SCOPE [--now MS] [--dry-run] [--json]
fabric-reconciliation explain   --store DIR --scope SCOPE [--now MS]
fabric-reconciliation dispatch  --store DIR --scope SCOPE [--now MS] [--plan-id ID]
fabric-reconciliation complete  --store DIR --attempt N --key HEX --state STATE
fabric-reconciliation verify    --store DIR --file VERIFICATION.json
fabric-reconciliation fence     --store DIR --scope SCOPE --below EPOCH [--subject ID]
fabric-reconciliation attempts  --store DIR [--scope SCOPE] [--json]
fabric-reconciliation outcomes  --store DIR [--scope SCOPE]
fabric-reconciliation serve     --store DIR [--address A] [--port N] [--ready-file F]
fabric-reconciliation scenario  --store DIR [--now MS] [--out FILE]
fabric-reconciliation selftest
```

`scenario` builds a deterministic synthetic fixture, commits it, plans it, checks
that the plan is reproduced exactly, and exits non-zero if it is not.

`fabric-reconcile-worker` drives one reconciliation inside a single process
incarnation and can terminate itself at a named durable boundary
(`before_plan`, `before_dispatch`, `after_dispatch_before_ack`,
`before_completion_commit`, `after_apply_before_verify`). It exists so the
multiprocess proof can kill a real process at a meaningful point.

`fabric-reconcile-bench` measures completed work at several scales and reports
the classifier's own work counters next to wall-clock time.

Documents are strict JSON. Unknown members are refused rather than ignored, so a
typo in a fixture cannot silently change what is being tested.

## Evidence matrix

| Capability | Status | How it is proved |
| --- | --- | --- |
| Comparison, classification, planning | **REAL** | implemented and exercised by the unit, drift, plan, adversarial, scale and differential suites |
| Durable journal, snapshot, compaction, torn-tail recovery, integrity refusal | **REAL** | `fr_test_persistence` attacks every format directly |
| Restart and fencing semantics | **REAL** | `fr_test_engine` in process, `fr_test_multiprocess` with real killed processes |
| Framed protocol and loopback service | **REAL** | `fr_test_transport` over real loopback sockets; `fr_test_multiprocess` kills and restarts a real service process |
| Concurrency and shutdown | **REAL** | `fr_test_concurrency` with barriers, no sleeps; shutdown proved without any timeout |
| Multiprocess ownership exclusion | **REAL** | `fr_test_multiprocess` proves a second live process is refused |
| AddressSanitizer | **REAL on this host** | `/fsanitize=address`, 12/12 suites pass |
| Static analysis | **REAL on this host** | MSVC `/analyze` over the whole library |
| Physical fabric behaviour (switches, NICs, ASICs, RDMA, DPU, NVLink) | **UNSUPPORTED** | the runtime never executes mutations and no physical device was exercised. All fabric content in the tests is **SYNTHETIC** |
| Multi-node distributed deployment | **UNSUPPORTED** | the service binds loopback only; no multi-host deployment was exercised |
| Authenticated or confidential transport | **UNSUPPORTED** | deliberately outside the boundary; no claim is made |
| UBSan, GCC, Clang, Linux, macOS | **UNSUPPORTED on this host** | only MSVC 19.44 on Windows was available; the sources are portable C++20 but were not compiled with another toolchain here |

Every fixture in the test suites is labelled `SYNTHETIC` in its evidence class,
and that label is carried into every plan and explanation, so a plan can never
be mistaken for a statement about physical hardware.

## Test suites

12 executables, 186 cases. No suite sets or relies on a timeout of any kind; a
hanging test is a defect and fails by never returning.

| Suite | Cases | What it proves |
| --- | --- | --- |
| `fr_test_core` | 33 | identities, checked arithmetic, SHA-256 vectors, canonical encoding, attribute values, documents, policy, status text |
| `fr_test_json` | 23 | strictness and totality of the JSON reader, including a generated-document round-trip property |
| `fr_test_drift` | 21 | freshness rules, evidence selection, every drift class, coverage-gap semantics |
| `fr_test_plan` | 22 | idempotence, determinism, verdicts, explain surfaces, dispatch authority |
| `fr_test_adversarial` | 18 | stale/newer generations, duplicates, missing dependencies, conflicting publishers, partial apply, late and duplicate completions, fences, verification independence |
| `fr_test_persistence` | 21 | journal and snapshot adversarial inputs, torn tails, compaction, store identity, bounded history |
| `fr_test_engine` | 9 | restart in process: epoch advance, fencing, ambiguity, fresh-clone closure |
| `fr_test_transport` | 15 | frame codec adversarial inputs, real loopback sessions, session identity, shutdown release |
| `fr_test_concurrency` | 8 | concurrent commit, plan, dispatch and completion with deterministic barriers; lifecycle cycles |
| `fr_test_scale` | 4 | linear work counters at 2k/8k/32k/128k subjects, bounded retained state, plan sealing |
| `fr_test_reference` | 3 | differential testing against an independent slow reference classifier over 1500 seeded cases |
| `fr_test_multiprocess` | 9 | real separate processes killed at named durable boundaries and restarted |

## Scale

Measured with `fabric-reconcile-bench` on the release build, one process, 16
hardware threads available. Work counters are the primary evidence; wall-clock
time is reported alongside them.

| subjects | classify ms | plan ms | subject visits | attribute comparisons | decisions | actions |
| --- | --- | --- | --- | --- | --- | --- |
| 2 000 | 1.06 | 7.97 | 2 000 | 1 882 | 2 000 | 494 |
| 8 000 | 7.17 | 46.77 | 8 000 | 7 529 | 8 000 | 1 977 |
| 32 000 | 37.31 | 168.69 | 32 000 | 30 117 | 32 000 | 7 906 |
| 128 000 | 152.32 | 701.38 | 128 000 | 120 470 | 128 000 | 31 624 |

At every step the subject count grows by 4.00x and the classifier's work grows by
exactly 4.00x. An accidental quadratic implementation would show 16x. Retained
state stays bounded: evidence history per scope, attempts, outcomes, plans and
explanations all have configured ceilings, and compaction runs automatically
after a configured number of records.

## Known limitations

These are genuine and are not worked around.

1. **No physical fabric was exercised.** The runtime compares states and issues
   intents; it never talks to a switch, NIC or queue. Every scenario in this
   repository is SYNTHETIC.
2. **MSVC on Windows only.** The sources are portable C++20 and no
   platform-specific construct is used outside small areas of `src/net.cpp`,
   `src/journal.cpp`, `src/snapshot.cpp`, `src/store.cpp` and
   `src/identity.cpp`, but no other toolchain was available on this host, so
   Linux, macOS, GCC, Clang and UBSan are **UNSUPPORTED on this host**.
3. **No secure transport.** Frames are integrity-checked, not authenticated.
   Anyone who can reach the loopback port can speak the protocol. This is inside
   the boundary statement, not an oversight.
4. **Values outside the six supported attribute kinds are UNSUPPORTED.** Ordered
   collections, maps and binary blobs cannot be expressed in the document
   formats; the classifier reports `UNSUPPORTED` rather than approximating them,
   but no reader can currently produce such a value, so that path is defensive.
5. **A subject whose observation does not cover an intended attribute is
   INDETERMINATE.** The runtime refuses to guess even though the desired value is
   known, because acting would mean claiming a difference it cannot prove.
6. **The client uses a blocking connect.** Connecting to an endpoint that
   accepts no connections and sends no reset can block. The service itself binds
   loopback and every other wait in the runtime is released deterministically.
7. **Observation duplicate detection is bounded by retention.** An observation
   identical to one that has already been evicted from the retained history is
   accepted as new. Its evidence class, generation and content are still fully
   validated.
8. **The plan bound is a bound.** When `max_plan_actions` is reached the plan is
   explicitly truncated and its verdict becomes `SEARCH_LIMIT_REACHED`; it never
   claims completeness it does not have.
9. **Explain text is a rendering, not a contract.** The stable machine surface is
   the canonical JSON produced by `--json` and the plan digest.

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
