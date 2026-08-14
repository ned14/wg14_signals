# Ideas and techniques to adopt from `wg14_atomic_waits` — concretised

Review date: 2026-08-06 (concretised against the `wg14_signals` implementation source);
2026-08-14 status reconciliation (done items removed, the new analysis.md AB1 finding
folded in); 2026-08-14 second status reconciliation (the eleven `[DONE 2026-08-14]`
items — 1.3, 2.1, 2.2, 2.3, 2.5, 3.4, 3.5, 4.2, 4.3, 4.4, 6.3 — removed from this file,
their verification notes moving with the analysis.md findings they fixed, plus the new
analysis.md AC2 finding folded into §5.10); 2026-08-14 Linux LSan pass (analysis.md AB1
and AC3 fixed, recorded in §5.9); 2026-08-14 Fil-C build-failure pass (analysis.md AC4
fixed in §5.11); 2026-08-14 Windows CI build-failure pass (analysis.md 4.10 fixed — MSVC
C++14/17 lacks `__VA_OPT__`; the header's sigfence counting now falls back to a plain
comma-list form there, after a wrong `/Zc:__VAOPT__` flag attempt was reverted);
2026-08-14 FreeBSD CI build-failure pass (clang 19 renamed the zero-arg variadic-macro
warning to `-Wc23-extensions`; the sigfence test now suppresses all three names);
2026-08-14 fix pass (analysis.md 2.5 recorded in §5.3; analysis.md 2.10/V2 — the Windows
vectored-handler NULL-tss guard — recorded in §5.8, done; analysis.md 2.12/V4 — the
Windows `stdc_raise` user-defined exception-code mapping — recorded in §5.8, done;
analysis.md 3.7 — `stdc_raise` now reports TSS-setup failure via errno; analysis.md 3.8 —
a partial `siginstall` now rolls back its already-installed signals, with a new
`--wrap=calloc` white-box regression test; analysis.md 3.15/V5 — the Windows vectored
function now dedups the global-decider pass across the unhandled-filter + continue-handler
pair, recorded in §5.8; analysis.md 3.18/X4 — Windows now maps
`EXCEPTION_STACK_OVERFLOW` to SIGSEGV like POSIX, recorded in §5.8). Source reviewed: `../wg14_atomic_waits` at `8b40d4d` (HEAD), which was
derived from this project. Every idea is tied to a specific file and function in this
tree, shows the current code, and gives the concrete replacement (or a test design that
would have caught the defect). Sibling references are cited as
`../wg14_atomic_waits/include/...` / `.../CMakeLists.txt` and verified by direct reading.

Cross-references to `plans/analysis.md` findings use its stable IDs (e.g. "2.6", "V2",
"AB1", "AC2"); `analysis.md` records verified reproductions inline.

---

## 4. Header and macro techniques

### 4.1 `WG14_SIGNALS_ATOMIC_PREFIX` in `config.h` (C/C++ single codebase)

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
(analysis.md 4.9) and guarantees the `extern "C"` declarations in the headers are
genuinely compiled from C++ TUs.

---

## 5. Implementation techniques (concrete defect fixes)

### 5.1 Make `tss_async_signal_safe_create/destroy/thread_init/get` NULL-safe (fixes analysis.md 2.6)

`tss_async_signal_safe_create/destroy/thread_init/get` do not validate their `val`
argument: a NULL or zeroed handle crashes (`tss_async_signal_safe_destroy(NULL)` ->
`LOCK(mem->lock)` on NULL), and `create` validates neither `val` nor `attr` (`attr ==
NULL` -> `memcpy` crash, `attr->create == NULL` crashes later in `thread_init`; analysis.md
2.6, X8, Z8). Return `-1`/`NULL` with `errno = EINVAL` when `val` is NULL.

**Verification.** On the fallback path, `stdc_raise(0, NULL, NULL)` and a bare
`sigguarded(...)` after a full install->uninstall cycle must not crash; also run on Linux
under `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` (the backend-override option added in
2026-08-14, formerly §2.1).

### 5.3 `tss_async_signal_safe_thread_init`: re-check under the lock (fixes analysis.md 3.13, 3.14; 2.5 done)

**Why.** `:209-249`: the user `create` callback runs outside `mem->lock`, then the map is
inserted without re-checking. Two racing threads both run `create` (leak of the loser's
value, double `count`, double atexit registration — 3.13); a `create` returning 0 with
NULL now reports failure (`-1`, `errno = EINVAL` — analysis.md 2.5, fixed 2026-08-14); a
failed `thread_atexit` leaves a committed entry and count (3.14).

**Concrete change.** Port the sibling's "capture/check under the lock" idiom
(`../wg14_atomic_waits/atomic_wait_common.ipp.ipp:556-589`): run `create` unlocked, then
re-lock, re-`get` the map, and if another thread won the race discard the copy (call
`mem->attr.destroy(newitem)`); register `thread_atexit` LAST, and if it fails roll back the
insert and count before returning -1. (The `ret == 0 && newitem == NULL` failure case is
already handled: analysis.md 2.5, fixed 2026-08-14.)

### 5.4 `tss_async_signal_safe_get`: lock-free read via a per-thread cached pointer

**Why.** `get` (`:255-272`) takes `mem->lock`, which is also taken inside signal-handler
context (`sig_global_tss_state` -> `tss_async_signal_safe_get`). A spinlock in a handler
deadlocks if the interrupted thread holds it (3.1) — so the "ASYNC-SIGNAL-SAFE" claim
(§7 of analysis.md) is false in general. The sibling's `current_thread_id()` solves
exactly this by caching in TLS and only falling back to a slow path on cache miss
(`../wg14_atomic_waits/current_thread_id.h:58-72`); `tss_async_signal_safe.c.ipp` already
does the same for the TID (`my_current_thread_id`, `:84-94`).

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
the §3.1 deadlock vector from the hot path). Keep the map write for `destroy`/deinit
bookkeeping.

### 5.5 POSIX `install_sighandler_impl`: fix `SA_NOCLDWAIT` and add `SA_RESTART` (fixes analysis.md 3.3)

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

### 5.7 `sigfillset_*` sets: drop `__attribute__((constructor))`, force init under the lock (fixes analysis.md 7.1, C11 compliance)

**Why.** `thrd_signal_handle_posix.c.ipp:49-67` (and the async sets) use
`static __attribute__((constructor))` — a GNU extension, violating AGENTS.md rule 1 — plus
a double-checked write race on `v` (7.1) for any platform where the constructor is
ignored.

**Concrete change.** Remove the attribute; build the three sets once from
`sig_global_tss_state_create()` (normal context, already under `state->lock`), and have
the `sigfillset_*` functions read only the pre-built statics (their own lazy init path
becomes a benign fallback for "called before any library call", which in a single-threaded
pre-main context cannot race). This keeps the functions read-only afterwards, preserving
the "ASYNC-SIGNAL-SAFE" claim.

### 5.8 Windows backend concrete fix (analysis.md V2) **[DONE 2026-08-14]**

- **NULL `tss->front` deref from a fresh thread (V2)** — the vectored handler calls
  `sig_global_tss_state()` on threads that never ran `sig_global_tss_state_init`; on the
  fallback path that returns NULL and `tss->front` dereferences it. **Fixed 2026-08-14:**
  the claim path now guards both `tss` and `tss->front`:

  ```c
  struct ... *tss = WG14_SIGNALS_PREFIX(sig_global_tss_state)();
  if(tss != WG14_SIGNALS_NULLPTR && tss->front != WG14_SIGNALS_NULLPTR)
  {
    longjmp(tss->front->buf, 1);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
  ```

  With no frame to resume (fresh thread), the handler falls through to the existing
  "generally end the process" path instead of NULL-deref'ing inside the exception
  handler. Verified: guard logic compiles/behaves correctly; full `ctest` suite passes on
  Linux and macOS.

- **`stdc_raise` aborts for unsupported signos (V4)** — `win32_exception_code_from_signal`
  hit `default: abort()` for SIGINT/SIGTERM/SIGPIPE/SIGUSR1 etc. **Fixed 2026-08-14:** the
  default case now maps the signo into the user-defined exception-code range
  (`0x40000000`-`0x7FFFFFFF`, mask `(DWORD) signo & 0x3FFFFFFF`), and
  `signal_from_win32_exception_code` reverses it (high-bit check: user codes are
  `>= 0x40000000` and `< 0x80000000`, all genuine system codes are `>= 0x80000000`).
  `stdc_raise(SIGINT)` now raises a valid SEH exception; the vectored handler dispatches a
  decider installed for it, or returns false via the software-raise-unclaimed path —
  POSIX parity. Negative signos round-trip as large positive values that the map
  bounds-checks as absent, so `stdc_raise(-1)` no-ops instead of aborting or reaching
  Windows Error Reporting. Verified: round-trip probe + full `ctest` suite on Linux and
  macOS.

- **Global deciders run twice per exception (V5)** — the same vectored function is
  registered as both the unhandled exception filter and the vectored continue handler, so
  on the no-debugger path Windows invokes it twice per exception (filter, then continue
  handler) and every side-effecting global decider runs twice. **Fixed 2026-08-14:** the
  function carries a per-thread dedup marker — the `EXCEPTION_RECORD` whose global-decider
  pass just ran and the decision it produced — and a second invocation for the same record
  returns the recorded decision immediately instead of re-running the deciders. The marker
  is `_Thread_local` (async-signal-safe on MSVC); each dispatch has its own record, so a
  nested exception runs the pass fresh; under a debugger only the continue handler runs,
  so the pass executes exactly once. Verified: `sigguarded_tss_init_test`'s Windows
  assertion tightened from `>= 1` to `== 1`; full `ctest` suite passes on Linux and macOS.

- **`EXCEPTION_STACK_OVERFLOW` not mapped to any signal (X4)** — a genuine stack overflow
  returned `signo == 0` -> `EXCEPTION_CONTINUE_SEARCH`, so WER terminated the process with
  no library involvement, while POSIX delivers SIGSEGV for a stack overflow.
  **Fixed 2026-08-14:** `signal_from_win32_exception_code` now maps
  `EXCEPTION_STACK_OVERFLOW` (0xC00000FD) to SIGSEGV, grouped with the access-violation
  case, exactly as POSIX. The reverse mapping is deliberately unchanged (`stdc_raise(SIGSEGV)`
  still raises an access violation — the asymmetry mirrors POSIX, where SIGSEGV is
  delivered for both fault classes). A decider recovering from a stack overflow must call
  `_resetstkoflw()` to restore the guard page. Verified: probe confirms the mapping and
  that all other codes are unchanged; full `ctest` suite passes on Linux and macOS.

### 5.9 `siguninstall`/`signal_decider_destroy` locking hygiene

- **`signal_decider_destroy` takes `state->lock` once per signal** (analysis.md §9):
  `:688-756` loops `LOCK/UNLOCK` around each signal's map access. Acquire once before the
  loop and release after.
- **`siguninstall` `-1` failure path leaks `ss`** (analysis.md §9): `:545-548`
  `return -1` before `free(ss)`; free first. (`uninstall_sighandler` always returns true so
  the path is currently dead, but keep it correct.)
- **`signal_decider_destroy` leaks the node on the normal destroy path (analysis.md
  2.24/AB1):** the AA1 fix left the post-unlock free as a *second* refcount decrement, so a
  node whose first decrement hit 0 (still-installed signal, base refcount 1) is
  LIST_REMOVE'd and then counted down to -1 and never freed. **Fixed 2026-08-14:** the
  map-entry refcount-zero branch now frees the node immediately (after the
  `sighandler_info_has_decider` walk) and sets `*retp = NULL`, leaving the post-unlock
  block for the map-miss path only — safe because the raise path bumps `refcount` under
  the same lock before its unlocked decider call. Verified under LSan on Linux arm64
  (clang/gcc x native/fallback, Debug/Release, C11/C23, shared ON/OFF): all 22 tests pass,
  where previously 9 failed with LeakSanitizer reports at `signal_decider_create:640`.
- **Fallback path: `sig_global_tss_state_create` leaks a pre-existing TSS (analysis.md
  2.25/AC3):** the function unconditionally assigns a fresh `tss_async_signal_safe` into
  the static slot, orphaning an existing TSS (e.g. one recreated by the documented
  post-uninstall `stdc_raise(0, ...)` setup call). **Fixed 2026-08-14:** return 0 (reuse)
  when the slot is already non-NULL, matching the async-safe path's lazy-init semantics.

### 5.10 C `thread_atexit`: propagate a failed `__cxa_thread_atexit()` registration (fixes analysis.md 3.21/AC2)

`thread_atexit.c.ipp:66-76` unconditionally `return 0` and drops the return of
`__cxa_thread_atexit()`. On macOS the return is unreliable, so the ignore-everything
behaviour must stay there; on glibc it is reliable and a dropped registration (ENOMEM)
silently loses the thread's cleanup — `tss_async_signal_safe_thread_init` reports success
while no deinit is scheduled (leaked map entry and `deinit_state`), and the async-safe
path's `sig_global_tss_state_init` leaves `*state` set with a leaked allocation.
Concrete change: probe at configure time whether the platform's `__cxa_thread_atexit`
return is trustworthy (extend the existing `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` probe),
propagate the return where trustworthy, and keep the ignore behaviour for macOS only.

### 5.11 `sigfence` volatile-sink fallback: explicitly cast away qualifiers (fixes analysis.md 2.9/AC4)

`thrd_signal_handle.h:193` — `WG14_SIGNALS_SIGFENCE_ESCAPE(a, i)` assigns `&(a)` into the
`void *volatile sigfence_sink[]`. When the argument is a `volatile` or `const`-qualified
lvalue (`volatile int result`, as in `test/thrd_sigfpe_test.c:98`), the implicit
conversion discards qualifiers and fails under `-Werror` on the volatile-sink path —
exactly the Fil-C CI leg, whose toolchain forces `DISABLE_INLINE_ASM=1`
(`cmake/filc-toolchain.cmake:32`). **Fixed 2026-08-14:** cast explicitly,
`sigfence_sink[(i)] = (void *) &(a)`; the sink's only purpose is to force the address to
escape into observable memory, so the cast is semantically correct. Verified with
`-DDISABLE_INLINE_ASM=1` under clang 18 `-Werror` for `volatile`/`const`/`volatile const`
arguments, plus the full 22-test `ctest` suite on Linux; the inline-asm path (`+m`
operands) accepts qualified lvalues and is unchanged.

---

## 6. Testing techniques (concrete designs)

### 6.1 `test_wait_until`: bounded spin handshake (anti-flake)

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

### 6.4 Remaining regression tests to add (each one maps to a verified analysis finding)

Add to `test/` (all `add_code_test`, C11):

| Test file | Exercise | Catches |
|---|---|---|
| `tss_null_handle_test.c` | `create/destroy/thread_init/get` on NULL and zeroed handles | 2.6 |
| `lock_whitebox_test.c` | `#include "detail/impl/lock_unlock.h"`, lock/unlock under TSan | 3.1 discipline |
| `decider_destroy_leak_test.c` | loop 10,000 x {create, destroy} on an installed signal; assert no node allocation growth (malloc/calloc interposition, the analysis.md AB1 pattern, now fixed — keep as a permanent regression guard) | AB1 |
| `thread_atexit_failure_test.c` | stub `__cxa_thread_atexit` via a linker/interposer shim returning -1 and assert `thread_atexit`/`thread_init` propagate the failure on platforms where the return is trustworthy | AC2 |

### 6.5 White-box tests (include internals directly) **[PARTIALLY DONE 2026-08-14]**

The sibling's `hash_table_whitebox_test.c` includes the backend `.ipp` with
`WG14_ATOMIC_WAITS_ENABLE_HEADER_ONLY` and drives internals single-threaded. For
`wg14_signals`, the white-box technique has already been applied to two regression tests
added for other findings: `install_sighandler_lock_test.c` (drives
`install_sighandler(SIGKILL)` and checks a fresh `LOCK(state->lock)` succeeds — would have
caught the original lock leak in minutes) and `signo_map_verstable_init_test.c` (forces
the NSIG >= 1024 branch and drives `signo_to_sighandler_map_t_get/insert/erase_itr`
directly). Still open:

- `lock_whitebox_test.c` — drives `LOCK/UNLOCK` directly under TSan as its own test.
- A `tss_map_whitebox_test.c` that includes `tss_async_signal_safe.c.ipp` and drives the
  map insert/erase/cleanup cycle single-threaded (the `thread_id_to_tls_map_t` verstable
  instance) — the map-under-TSan coverage that needs no threads.

### 6.6 Leak detection through the public API

Adapt the sibling's `wait_expected_leak_test.c` discriminator: instead of instrumentation,
observe API-visible behaviour. For `wg14_signals`: after many `siginstall`/`siguninstall`
cycles with deciders destroyed, a subsequent `stdc_raise` of a fully-uninstalled signal
must return false cleanly (no residual `deferred_frees` node reached). Concretely: loop
20,000 x {create decider, destroy decider} on SIGUSR1, then assert `siguninstall(handlers)`
and one final `stdc_raise(SIGUSR1) == false` under ASan/LSan — a leaked/kept node
surfaces as an ASan report or as a raise that "finds" a handler.

### 6.7 Compile-fail test suite (port `expect_compile_fail.cmake`)

**Concrete change.** Copy `../wg14_atomic_waits/test/expect_compile_fail.cmake` and the
`add_compile_fail_test` function from `../wg14_atomic_waits/test/CMakeLists.txt:87-110`
into `test/CMakeLists.txt`. Targets (each a `.c` + `.cpp` pair):

- `sigfence_too_many_args`: `sigfence(a,b,c,d,e,f,g,h,i)` (9 args) — must fail (the
  `__VA_OPT__` counter returns the 9th argument, producing an undefined
  `WG14_SIGNALS_SIGFENCE_IMPL_<expr>`); matches analysis.md X11.
- `sigfence_rvalue`: `sigfence(1)` on GNU/clang — `+m` operand must be an lvalue
  (analysis.md 2.8); intentionally GNU/clang-only (`#if` guard in the source).
- `windows_sigset_overflow`: after the 2026-08-14 sigset_t bounds-check (formerly §4.3)
  `sigaddset(&ss, 33)` becomes a
  no-op rather than a compile error, so instead make this a
  `WG14_SIGNALS_STATIC_ASSERT`-backed compile-fail for the `sizeof(sigset_t) <= 4` contract
  (the static-assert helper added in 2026-08-14, formerly §4.2) on Windows.
- MSVC C++ note: the sigfence argument-counting static assert needs `__VA_OPT__` for the
  zero-argument form; MSVC only provides it via the conforming preprocessor in C++20 and
  C11/C17 modes. The header now detects `__VA_OPT__` and falls back to a plain comma-list
  counting on MSVC C++14/17, which handles the 1..8-argument range the assert checks
  (analysis.md 4.10, fixed 2026-08-14); the zero-argument `sigfence()` is then unavailable
  on that configuration, and the compile-fail suite must respect the same detection.

The `expect_compile_fail.cmake` script matches the *literal* namespaced diagnostic text (no
regexes) and echoes build output, so failures are visible in ctest logs.

### 6.8 Benchmark structure (partially done)

`wg14_signals` benchmark targets are already excluded from CI via `ctest -E benchmark`. To
match the sibling: convert the two benchmark targets in `test/CMakeLists.txt:2,4` from
`add_code_test` to `add_code_example` with `PROPERTIES EXCLUDE_FROM_ALL TRUE` so they
aren't built by default at all, and document exact reproduce commands in Readme.md (the
sibling's pattern at `../wg14_atomic_waits/Readme.md:243-249`).

---

## 7. Documentation and process

### 7.1 `docs/proposal.md` — vendor the N3924 rev 4 wording

The sibling carries the WG14 wording it implements (`docs/proposal.md`); this repo
implements N3924 rev 4 but does not vendor it. Add `docs/proposal.md` with the N3924 rev 4
text so `plans/analysis.md` deviations can cite "§X of the wording" instead of prose.

### 7.2 `plans/test-review-todos.md` companion

Port the sibling's `plans/test-review-todos.md` structure: for *every* item in
`plans/analysis.md`, a verdict (testable / not testable / characterization-only /
CI-change) with the test design in §6.4-6.7, plus an explicit "do not naively complete
these" section for deliberately-untested behaviours (e.g. Windows SEH real-fault paths,
the SIGFPE trap behaviour noted in analysis.md 6.4).

### 7.4 `.gitattributes` trim

`wg14_signals/.gitattributes` (102 lines) references `cmake/headers.cmake`,
`cmake/interface.cmake`, `cmake/sources.cmake`, `cmake/tests.cmake` which do not exist.
Trim to the sibling's 12-line shape (`../wg14_atomic_waits/.gitattributes`).

---

## 8. What NOT to adopt (cautionary notes)

- **The `cmake_minimum_required(3.15)` + `PROJECT_IS_TOP_LEVEL` mismatch is shared by both
  projects** (3.21 feature; analysis.md W6). Fix it here (bump minimum to 3.21 or replace
  the guard); don't copy the sibling's latent defect.
- **The sibling's `ALWAYS_USE_PTHREADS_BACKEND` cannot build on Windows/MSVC** (no
  `<pthread.h>`); their docs still claim "every platform". The `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS`
  option that was adopted instead (done 2026-08-14) has no such problem — it is pure
  compile-definition — but note that forcing the fallback on Windows exercises
  `tss_async_signal_safe` with MSVC's `/experimental:c11atomics`.
- **The hash-table proxy engine is tuned for wait/notify**; adopt the *techniques*
  (§5.2-5.4: re-check-under-lock, refcounted containers, lock-free cached reads) rather
  than the structure wholesale. Replacing `verstable.h` with a triangular-probing table is
  a larger refactor; the concrete bug that motivated it (analysis.md Z1: uninitialised
  verstable map on NSIG>=1024) was fixed exactly as recommended here — initialising the
  map instance in `sig_global_state()` — so there is no longer a defect pushing the
  replacement.
- **CI breadth**: 7 jobs/~80 legs in the sibling is proportional to its problem; the
  fallback-TLS matrix dimension added in 2026-08-14 (32 Linux + 16 macOS legs) is the
  current ceiling — do not widen it further without a concrete defect motivating it.

---

## 9. Priority-ordered adoption plan

| # | Change | Location | Fixes (analysis.md) | Effort |
|---|--------|----------|--------------------|--------|
| 1 | Fallback-path setup: NULL-safe tss API | `tss_async_signal_safe.c.ipp:96-272` | 2.6 | Small |
| 2 | `tss_async_signal_safe`: lock-free `get` via cached TLS value; init re-check | `tss_async_signal_safe.c.ipp:136-243` | 3.1, 3.13, 3.14 | Medium |
| 3 | `install_sighandler` flags: drop `SA_NOCLDWAIT`, add `SA_RESTART` | `thrd_signal_handle_posix.c.ipp:371-383` | 3.3 | Small |
| 4 | Remaining regression tests (tss NULL, lock whitebox, decider-destroy leak) | `test/*` | 2.6, 3.1 | Medium |
| 5 | Compile-fail suite (`expect_compile_fail.cmake` + sigfence targets) | `test/` | 5.3, 2.8, X11 | Medium |
| 6 | `sigfillset_*` constructor-attribute removal + init under lock | `thrd_signal_handle_posix.c.ipp:49-127` | 7.1, C11 rule 1 | Small |
| 7 | C `thread_atexit`: propagate a failed `__cxa_thread_atexit()` registration (macOS only excepted) | `thread_atexit.c.ipp:66-76` | 3.21/AC2 | Small |
| 8 | `docs/proposal.md` + `plans/test-review-todos.md` + `.gitattributes` trim | `docs/`, `plans/`, `.gitattributes` | process | Small |
