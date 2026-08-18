# Ideas and techniques to adopt from `wg14_atomic_waits` — concretised

Review date: 2026-08-06 (concretised against the `wg14_signals` implementation source);
2026-08-14 reconciliation, merge, and renumbering passes; items renumbered to 1-11 in §5
priority order. Source reviewed: `../wg14_atomic_waits` at `8b40d4d` (HEAD), derived from
this project. Every idea is tied to a specific file and function in this tree, shows the
current code, and gives the concrete replacement (or a test design that would have caught
the defect).

Cross-references to `plans/analysis.md` findings use each finding's stable four-letter
code (e.g. `SPIN`), never the priority row number, which changes when the order is
revised; `analysis.md` records verified reproductions inline.

---

## Ideas and techniques, in priority order

## 1. Implementation techniques (concrete defect fixes)

### 1 `tss_async_signal_safe` TSS core rework: lock-free `get` + `thread_init` re-check under the lock (fixes `SPIN`, `REEN`, `TAFL`)

**Why.** `get` (`:255-272`) takes `mem->lock`, which is also taken inside signal-handler
context (`sig_global_tss_state` -> `tss_async_signal_safe_get`). A spinlock in a handler
deadlocks if the interrupted thread holds it (`SPIN`) — so the "ASYNC-SIGNAL-SAFE" claim
(§2 of analysis.md) is false in general. `thread_init` (`:209-249`) has the related
lifecycle bugs: the user `create` callback runs outside `mem->lock`, then the map is
inserted without re-checking — two racing threads both run `create` (leak of the loser's
value, double `count`, double atexit registration — `REEN`) — and a failed `thread_atexit`
leaves a committed entry and count (`TAFL`). The sibling's `current_thread_id()` solves
exactly the `get` problem by caching in TLS and only falling back to a slow path on cache
miss (`../wg14_atomic_waits/current_thread_id.h:58-72`); `tss_async_signal_safe.c.ipp`
already does the same for the TID (`my_current_thread_id`, `:84-94`).

**Concrete change.** Cache the per-thread *value* in a plain `WG14_SIGNALS_THREAD_LOCAL`
slot set by `thread_init` (outside handler context), and make `get` read only the cache:

```c
static WG14_SIGNALS_THREAD_LOCAL void *my_value;   /* set in thread_init */

void *WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(tss_async_signal_safe val)
{
  (void) val;
  return my_value;
}
```

`my_value` is written only by `thread_init` on the thread itself, so the read is lock-free
and race-free; the hash table remains solely for `destroy()`'s iteration over all threads.
This makes the async-signal-safety claim *actually true* on the fallback path (and removes
the `SPIN` deadlock vector from the hot path). Keep the map write for `destroy`/deinit
bookkeeping.

For `thread_init`, port the sibling's "capture/check under the lock" idiom
(`../wg14_atomic_waits/atomic_wait_common.ipp.ipp:556-589`): run `create` unlocked, then
re-lock, re-`get` the map, and if another thread won the race discard the copy (call
`mem->attr.destroy(newitem)`); register `thread_atexit` LAST, and if it fails roll back the
insert and count before returning -1. (The `ret == 0 && newitem == NULL` failure case is
already handled — fixed 2026-08-14.)

### 2 POSIX `install_sighandler_impl`: fix `SA_NOCLDWAIT` and add `SA_RESTART` (fixes `FLGS`)

**Why.** `thrd_signal_handle_posix.c.ipp:371-383` installs
`SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER` for *every* signal. `SA_NOCLDWAIT` on SIGCHLD
changes process child-reaping semantics (auto-reap; `waitpid` -> ECHILD) for the whole
tenure of the install. No `SA_RESTART` means syscalls return EINTR during the tenure even
if the pre-existing handler had SA_RESTART.

**Concrete change.** Compute flags per signal:

```c
sa.sa_sigaction = WG14_SIGNALS_PREFIX(raw_signal_handler);
sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;   /* no SA_NOCLDWAIT */
```

i.e. add `SA_RESTART` unconditionally and *never* set `SA_NOCLDWAIT` (if the pre-existing
handler had it, it's preserved in `old_handler` and restored on uninstall anyway, and it
only ever applied to SIGCHLD). Document the `SA_NODEFER` re-entrancy trade-off in the
header (already partially documented at `thrd_signal_handle.h:550-552`).

### 3 `sigfillset_*` sets: drop `__attribute__((constructor))`, force init under the lock (fixes `SIGF`, C11 compliance)

**Why.** `thrd_signal_handle_posix.c.ipp:49-67` (and the async sets) use
`static __attribute__((constructor))` — a GNU extension, violating AGENTS.md rule 1 — plus
a double-checked write race on `v` (`SIGF`) for any platform where the constructor is
ignored.

**Concrete change.** Remove the attribute; build the three sets once from
`sig_global_tss_state_create()` (normal context, already under `state->lock`), and have
the `sigfillset_*` functions read only the pre-built statics (their own lazy init path
becomes a benign fallback for "called before any library call", which in a single-threaded
pre-main context cannot race). This keeps the functions read-only afterwards, preserving
the "ASYNC-SIGNAL-SAFE" claim.

### 4 C `thread_atexit`: propagate a failed `__cxa_thread_atexit()` registration (fixes `CXAT`)

`thread_atexit.c.ipp:66-76` unconditionally `return 0` and drops the return of
`__cxa_thread_atexit()`. On macOS the return is unreliable, so the ignore-everything
behaviour must stay there; on glibc it is reliable and a dropped registration (ENOMEM)
silently loses the thread's cleanup — `tss_async_signal_safe_thread_init` reports success
while no deinit is scheduled (leaked map entry and `deinit_state`), and the async-safe
path's `sig_global_tss_state_init` leaves `*state` set with a leaked allocation.
Concrete change: probe at configure time whether the platform's `__cxa_thread_atexit`
return is trustworthy (extend the existing `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` probe),
propagate the return where trustworthy, and keep the ignore behaviour for macOS only.

### 5 `siguninstall`/`signal_decider_destroy` locking hygiene

- **`signal_decider_destroy` takes `state->lock` once per signal** (analysis.md §3):
  `:688-756` loops `LOCK/UNLOCK` around each signal's map access. Acquire once before the
  loop and release after.
- **`siguninstall` `-1` failure path leaks `ss`** (analysis.md §3): `:545-548`
  `return -1` before `free(ss)`; free first. (`uninstall_sighandler` always returns true so
  the path is currently dead, but keep it correct.)

---

## 2. Header and macro techniques

### 6 `WG14_SIGNALS_ATOMIC_PREFIX` in `config.h` (C/C++ single codebase)

**Why.** `lock_unlock.h:27-31` defines `WG14_SIGNALS_ATOMIC_PREFIX` locally;
`tss_async_signal_safe.c.ipp` uses the awkward `std::`-line-split idiom (`:60-64`,
`:70-73`) because the prefix isn't available there; the public headers don't use it at
all. The sibling centralises it (`../wg14_atomic_waits/atomic_wait.h:66-74`).

**Concrete change.** Move to `config.h`:

```c
#ifndef WG14_SIGNALS_ATOMIC_PREFIX
#ifdef __cplusplus
#include <atomic>
#define WG14_SIGNALS_ATOMIC_PREFIX std::
#else
#define WG14_SIGNALS_ATOMIC_PREFIX
#endif
#endif
```

Delete the local definition from `lock_unlock.h`, and rewrite the
`atomic_uint`/`std::atomic_uint` split in `tss_async_signal_safe.c.ipp:58-76` to use
`WG14_SIGNALS_ATOMIC_PREFIX atomic_uint count;` etc. This eliminates the C/C++ drift
(`SJSP`) and guarantees the `extern "C"` declarations in the headers are
genuinely compiled from C++ TUs.

---

## 3. Testing techniques (concrete designs)

### 7 `test_wait_until`: bounded spin handshake (anti-flake)

**Concrete change.** Port `../wg14_atomic_waits/test/test_common.h:109-138` into
`test/test_common.h` verbatim (adapted to `WG14_SIGNALS_*` names): a
`test_wait_until(const char *what, const atomic_int *value, int goal)` that spins with
`timespec_get` deadline and `abort()`s after 2000 ms with a named diagnostic. Rewrite the
`while(atomic_load(...) == 2) {}` handshakes in `thrd_signal_handle_test.c:118-120` and
`thrd_sigfpe_test.c` to use it. The same unbounded pattern is used by the newer tests too:
`siguninstall_raise_test.c:124-126` (`decider_entered` wait) and
`tss_concurrent_exit_test.c:60-63` (`concurrent_exit_barrier` wait) both spin forever if
the coordinating thread dies first — ctest's `TIMEOUT 60` catches the hang, but only
after a full minute with no phase information. (AGENTS.md rule 5.)

### 8 Remaining regression tests to add (each one maps to a verified analysis finding)

Add to `test/` (all `add_code_test`, C11):

| Test file | Exercise | Catches |
|---|---|---|
| `lock_whitebox_test.c` | `#include "detail/impl/lock_unlock.h"`, lock/unlock under TSan | `SPIN` discipline |
| `decider_destroy_leak_test.c` | loop 10,000 x {create, destroy} on an installed signal; assert no node allocation growth (malloc/calloc interposition — permanent regression guard for the decider-destroy node leak fixed 2026-08-14) | `LEAK` |
| `thread_atexit_failure_test.c` | stub `__cxa_thread_atexit` via a linker/interposer shim returning -1 and assert `thread_atexit`/`thread_init` propagate the failure on platforms where the return is trustworthy | `CXAT` |
| `leak_detection_test.c` | loop 20,000 x {create decider, destroy decider} on SIGUSR1, then assert `siguninstall(handlers)` and one final `stdc_raise(SIGUSR1) == false` under ASan/LSan — API-observable variant of `decider_destroy_leak_test`: a leaked/kept node surfaces as an ASan report or as a raise that "finds" a handler | `SPIN` guard |

(`TSSD` is wontfix, 2026-08-16: double-destroy is documented undefined behaviour per the
C11/POSIX/N3924 contract, so no regression guard is written for it — see
`plans/analysis.md`.)

### 9 White-box tests (include internals directly)

The sibling's `hash_table_whitebox_test.c` includes the backend `.ipp` with
`WG14_ATOMIC_WAITS_ENABLE_HEADER_ONLY` and drives internals single-threaded. For
`wg14_signals`, the white-box technique has been applied to two regression tests added
for other findings: `install_sighandler_lock_test.c` (drives
`install_sighandler(SIGKILL)` and checks a fresh `LOCK(state->lock)` succeeds — would have
caught the original lock leak in minutes) and `signo_map_verstable_init_test.c` (forces
the NSIG >= 1024 branch and drives `signo_to_sighandler_map_t_get/insert/erase_itr`
directly). Still open:

- `lock_whitebox_test.c` — drives `LOCK/UNLOCK` directly under TSan as its own test.
- A `tss_map_whitebox_test.c` that includes `tss_async_signal_safe.c.ipp` and drives the
  map insert/erase/cleanup cycle single-threaded (the `thread_id_to_tls_map_t` verstable
  instance) — the map-under-TSan coverage that needs no threads.

### 10 Compile-fail test suite (port `expect_compile_fail.cmake`)

**Concrete change.** Copy `../wg14_atomic_waits/test/expect_compile_fail.cmake` and the
`add_compile_fail_test` function from `../wg14_atomic_waits/test/CMakeLists.txt:87-110`
into `test/CMakeLists.txt`. Targets (each a `.c` + `.cpp` pair):

- `sigfence_too_many_args`: `sigfence(a,b,c,d,e,f,g,h,i)` (9 args) — must fail (the
  `__VA_OPT__` counter returns the 9th argument, producing an undefined
  `WG14_SIGNALS_SIGFENCE_IMPL_<expr>`); matches `SFAR`.
- `sigfence_rvalue`: `sigfence(1)` on GNU/clang — `+m` operand must be an lvalue
  (`SFQL`); intentionally GNU/clang-only (`#if` guard in the source).
- `windows_sigset_overflow`: after the 2026-08-14 sigset_t bounds-check
  `sigaddset(&ss, 33)` becomes a
  no-op rather than a compile error, so instead make this a
  `WG14_SIGNALS_STATIC_ASSERT`-backed compile-fail for the `sizeof(sigset_t) <= 4` contract
  (the static-assert helper added in 2026-08-14) on Windows.
- MSVC C++ note: the sigfence argument-counting static assert needs `__VA_OPT__` for the
  zero-argument form; MSVC only provides it via the conforming preprocessor in C++20 and
  C11/C17 modes. The header detects `__VA_OPT__` and falls back to a plain comma-list
  counting on MSVC C++14/17, which handles the 1..8-argument range the assert checks; the
  zero-argument `sigfence()` is then unavailable on that configuration, and the compile-fail
  suite must respect the same detection.

The `expect_compile_fail.cmake` script matches the *literal* namespaced diagnostic text (no
regexes) and echoes build output, so failures are visible in ctest logs.

### 11 Benchmark structure (partially done)

`wg14_signals` benchmark targets are already excluded from CI via `ctest -E benchmark`. To
match the sibling: convert the two benchmark targets in `test/CMakeLists.txt:2,4` from
`add_code_test` to `add_code_example` with `PROPERTIES EXCLUDE_FROM_ALL TRUE` so they
aren't built by default at all, and document exact reproduce commands in Readme.md (the
sibling's pattern at `../wg14_atomic_waits/Readme.md:243-249`).

---

## 4. What NOT to adopt (cautionary notes)

- **The `cmake_minimum_required(3.15)` + `PROJECT_IS_TOP_LEVEL` mismatch is shared by both
  projects** (3.21 feature; `TOPL`). Fix it here (bump minimum to 3.21 or replace
  the guard); don't copy the sibling's latent defect.
- **The sibling's `ALWAYS_USE_PTHREADS_BACKEND` cannot build on Windows/MSVC** (no
  `<pthread.h>`); their docs still claim "every platform". The `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS`
  option that was adopted instead (done 2026-08-14) has no such problem — it is pure
  compile-definition — but note that forcing the fallback on Windows exercises
  `tss_async_signal_safe` with MSVC's `/experimental:c11atomics`.
- **The hash-table proxy engine is tuned for wait/notify**; adopt the *techniques*
  (item 1: re-check-under-lock, lock-free cached reads) rather
  than the structure wholesale. Replacing `verstable.h` with a triangular-probing table is
  a larger refactor; the concrete bug that motivated it (an uninitialised
  verstable map on NSIG>=1024) was fixed exactly as recommended here — initialising the
  map instance in `sig_global_state()` — so there is no longer a defect pushing the
  replacement.
- **CI breadth**: 7 jobs/~80 legs in the sibling is proportional to its problem; the
  fallback-TLS matrix dimension added in 2026-08-14 (32 Linux + 16 macOS legs) is the
  current ceiling — do not widen it further without a concrete defect motivating it.

---

## 5. Priority-ordered adoption plan

The adoption order is driven by (a) the priority of the `analysis.md` findings each
change fixes and (b) impact-to-effort ratio, grouped into: fixes to the library core,
small high-value behavioural fixes, test-suite hardening, regression tests that guard the
top findings, then CI and process hygiene.

**Why this order.**
- **The TSS core rework leads (item 1).** The lock-free per-thread cached `get` is the
  single change that makes the fallback path's "ASYNC-SIGNAL-SAFE" claim true: it removes
   the top-tier spinlock-in-handler deadlock vector (`SPIN`) and, with the
   re-check-under-lock init, the `REEN`/`TAFL` lifecycle leaks — three findings (`SPIN`, `REEN`, `TAFL`)
   with one coherent change, which outranks any
   single-finding fix below it.
- **Small fixes to Med findings are pulled up (item 2).** Dropping `SA_NOCLDWAIT` and
  adding `SA_RESTART` fixes the Med `FLGS` finding (silent `waitpid`/`EINTR` alteration on
  every default `siginstall(NULL)`) with Small effort, so it precedes the Low/C11
  `sigfillset` cleanup (item 3, fixes `SIGF`), the Low failure-propagation fix (item 4,
  fixes `CXAT`), and the lock and C/C++ hygiene items (5-6, fixing §3 minors and `SJSP`)
  — each of which fixes findings lower in the analysis.md priority order.
- **Stabilise the existing tests before adding new ones (item 7).** `test_wait_until`
  converts the unbounded spin handshakes into bounded, named-diagnostic waits (AGENTS.md
  rule 5), so the suite cannot hang for a minute before ctest's timeout silently.
- **Regression tests then guard the top findings (items 8-10), ordered by the priority
  of what they guard:** spinlock discipline (`SPIN`), then `thread_atexit` failure
  propagation (`CXAT`), then header/compile behaviours (`SFAR`, `SFQL`, `NDBS`).
- **CI/benchmark hygiene last (item 11):** no runtime impact; worthwhile only once the
  code fixes and their tests are in place.

| # | Category | Priority | Change | Fixes (analysis.md) | Effort |
|---|----------|----------|--------|--------------------|--------|
| 1 | async | Med | `tss_async_signal_safe`: lock-free `get` via cached TLS value; init re-check | `SPIN`, `REEN`, `TAFL` | Medium |
| 2 | semantics | Med | `install_sighandler` flags: drop `SA_NOCLDWAIT`, add `SA_RESTART` | `FLGS` | Small |
| 3 | race | Low | `sigfillset_*` constructor-attribute removal + init under lock | `SIGF`, AGENTS rule 1 | Small |
| 4 | error | Low | C `thread_atexit`: propagate a failed `__cxa_thread_atexit()` registration (macOS only excepted) | `CXAT` | Small |
| 5 | locking | Low | `siguninstall`/`signal_decider_destroy` locking hygiene (O(NSIG) locks, `-1` leak) | §3 minor | Small |
| 6 | cpp | Low | `WG14_SIGNALS_ATOMIC_PREFIX` centralised in `config.h` | `SJSP` | Small |
| 7 | test | Low | `test_wait_until` bounded handshake (AGENTS rule 5) | test hygiene | Small |
| 8 | test | Med | Remaining regression tests (thread_atexit failure, API leak detection) | `SPIN`, `CXAT` | Medium |
| 9 | test | Med | White-box tests (`lock_whitebox_test`, `tss_map_whitebox_test`) | `SPIN` discipline | Medium |
| 10 | test | Low | Compile-fail suite (`expect_compile_fail.cmake`) | `SFAR`, `SFQL`, `NDBS` | Medium |
| 11 | build | Low | Benchmark structure (`EXCLUDE_FROM_ALL`) | CI hygiene | Small |
