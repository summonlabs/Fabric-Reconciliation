# Concurrency and ownership audit

Scope: the whole runtime, inspected by hand rather than inferred from tests.
Every item below was checked against the source at the closure commit. Items
marked **fixed** were reproducible defects found by this audit and repaired
before release.

## Lock inventory

| Lock | Guards | Acquired by |
| --- | --- | --- |
| `ReconciliationEngine::mutex_` | the durable store, every live table, the statistics counters, the live epoch and boot identity | every public engine method, exactly once, at entry |
| `ReconciliationServer::Impl::mutex` | the session registry, the session counter and the accept-thread handle list | the accept thread, `sessions()`, `Stop()` |
| `Socket::handle_` (`std::atomic<void*>`) | the descriptor value | lock-free; an atomic exchange makes close exactly-once |

There are no other locks, no condition variables and no lock hierarchies.

## Audit items

### 1. Read-lock then write-lock re-entry on the same lock

Not present. The runtime uses `std::mutex` only; there is no reader/writer lock
and therefore no upgrade path. **Checked:** every call site of `mutex_` is a
`std::lock_guard` taken at the top of a public method.

### 2. Write lock held across helper or callback paths that re-enter state

Not present, and enforced by construction:

* the engine exposes no callbacks, no listener interfaces and no logging hooks;
* every helper that touches state takes the form `...Locked` and is documented
  as requiring the caller to hold `mutex_`;
* the only re-entrant-looking helpers (`MaybeCompactLocked`) call the store
  directly and never a public engine method.

**Checked:** `grep -n "lock_guard" src/engine.cpp` shows one acquisition per
public method; `MaybeCompactLocked` is the only nested call and it does not
re-enter the engine.

### 3. Event, log or callback invocation beneath internal locks

Not present. The runtime has no event bus, no observer registry and no
user-supplied callable anywhere in the public API. Diagnostics are returned as
values, not pushed.

### 4. Joining workers while holding state they need

Not present. `ReconciliationServer::Stop()` is deliberately structured so that
no join happens under `Impl::mutex`:

1. set `stopping`;
2. signal the accept wake channel and every session wake channel;
3. copy the session list **under** the mutex, then release it;
4. join the accept thread and every session thread with **no lock held**;
5. only then reacquire the mutex to clear the registry.

### 5. Cancellation and shutdown with reversed lock ordering

Not present. There is no path that takes `Engine::mutex_` and then
`Impl::mutex` or the reverse: the server never calls the engine while holding
its own mutex (see item 7), and the engine never calls the server at all.

### 6. Blocked socket and thread teardown

**fixed.** A blocking `recv` is not guaranteed to return when the socket is shut
down from another thread on every supported platform: on Windows,
`shutdown()` on a listening socket returns `WSAENOTCONN` and a pending
`accept()` is not released. The first implementation relied on that behaviour
and `ReconciliationServer::Stop()` deadlocked while joining a session thread
that was blocked in `recv`.

The runtime now releases every blocking wait through an explicit loopback wake
channel that the waiting thread selects on together with its socket:

* the accept loop waits on the listener and the listener wake channel;
* each session waits on its own connection and its own wake channel;
* the client waits on its connection and its own wake channel;
* `Stop()` and `Close()` only signal; the owning thread performs the close.

No timeout, no polling and no reliance on platform-specific unblocking
guarantees is involved, which is why the shutdown tests can run without any
watchdog.

### 7. Cross-object mutex order inversion

Not present. The only nesting is server-then-nothing: `Impl::ServeSession`
calls `Handle`, which calls the engine, and it does so with `Impl::mutex`
**released**. The mutex is used in exactly three short critical sections (append
a session, copy the session list, clear the session list).

### 8. Moved-from handle ownership

**fixed.** `ReconciliationClient` originally held a raw `void*` to its socket and
deleted it in `Close()`. A move left the source holding a dangling pointer that
its destructor would use. The client now owns its socket and its wake channel
through `std::shared_ptr`, so a call already in flight keeps the descriptor
alive while `Close()` releases the owner's reference, and a moved-from client
holds nothing.

### 9. Close and shutdown races, double close

**fixed.** `Socket::Close()` now uses an atomic exchange, so exactly one thread
wins the right to close a descriptor; a concurrent `Shutdown()` only loads the
handle and is a no-op once the winner has taken it. Before this change a
shutdown racing the owning thread could close the same descriptor twice, which
can close an unrelated descriptor that the process has since opened.

### 10. Callbacks or references to mutable state retained beyond lock lifetime

Not present in the API. The one place where a reference escapes is
`ReconciliationEngine::boot_report()`, which returns a reference to an immutable
snapshot written once during `Boot`; it is never mutated afterwards, so a
reference cannot observe a torn value.

Pointers into the durable tables (`state().intents`, `state().attempts`) are
never returned to callers. Every accessor copies. Inside the engine, the two
places that iterate and then mutate (`CommitRestartFencing` and `PutFence`)
first copy the candidates into a local vector, so no map iterator is invalidated
by the mutation that follows.

### 11. Store ownership and two live writers

**fixed.** Two processes appending to one journal would interleave records and
break the contiguity the format guarantees. The store now takes an exclusive
operating-system lock on `<store>/store.lock` for its whole lifetime. The lock
is released by the operating system when the process exits for any reason,
including a hard kill, so no stale-lock heuristic exists and none can be wrong.
A second live process is refused with an explicit
`CONFLICT(DURABLE_WRITE_FAILED)` rather than being allowed to corrupt the log.
This is proved in `fr_test_multiprocess`.

### 12. Invocation of the engine with a partially constructed session

The session identity (`session_id`, `client_boot`, `handshake_epoch`) is written
under `Impl::mutex` before the session thread can observe a request, and the
request handler re-validates the claimed boot identity and epoch against live
authority on every request. A session can therefore never act under another
session's identity, boot or epoch even if the registry is read concurrently.

### 13. Engine lock held while acquiring the store lock

The store lock is acquired once, during `Boot`, before the engine is published
to any caller and therefore before any other thread can reach `mutex_`. There is
no path that takes the engine mutex and then blocks on filesystem ownership.

## Result

Thirteen audit items inspected; four reproducible defects found and fixed
(items 6, 8, 9, 11). The fixes are covered by `fr_test_concurrency`,
`fr_test_transport` and `fr_test_multiprocess`, which run without any timeout
or watchdog: a hang in any of them fails the suite by never returning, which is
the intended signal.
