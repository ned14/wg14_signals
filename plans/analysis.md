# Exhaustive implementation analysis: wg14_signals

Review dates: 2026-08-05 and 2026-08-06 (seven full review passes, all against the same
revision); 2026-08-14 status reconciliation pass against the current tree (fixed
findings removed, one new finding — AB1 — added); 2026-08-14 second status
reconciliation against the current tree (fixed findings removed: V3, 4.5, 6.6/AA3, 7.2,
and the fixed portions of 4.6, 5.2, 5.3, 5.4; one new finding — AC2 — added); 2026-08-14
Linux LSan pass against the current tree (fixed: AB1 and one new fallback-path finding
AC3; one new finding — AC1 docs-hygiene — added to §9); 2026-08-14 Fil-C build-failure
pass (one new finding — AC4 — fixed, in the sigfence volatile-sink fallback);
2026-08-14 Windows CI build-failure pass (fixed analysis.md 4.10 — MSVC C++ needs
`/Zc:__VAOPT__`). Original
revision reviewed: `f48e95e` ("Implement all the changes as per N3924 WIP wording for
'Thread-safe signals handling rev 4'"), plus one uncommitted whitespace/`nullptr`-for-C++
change in `config.h`.

Scope: every header, every source file, both backends (POSIX/Windows), the header-only
configuration, the fallback hash-table TLS path and the async-signal-safe TLS path, all
error paths, and all build configurations exercised and *not* exercised by CI.

Findings are ranked by severity. Items marked **[confirmed]** were reproduced on macOS
(arm64, clang 17, ASan/UBSan where noted). Windows-only items are code-level findings (no
Windows host available) but were verified against the MSVC build matrix in CI. Finding
IDs are stable and are referenced by `plans/ideas.md`. Later-pass findings keep their
pass letter (V = pass 2, W = pass 3, X = pass 4, Y = pass 5, Z = pass 6, AA = pass 7,
AB = pass 8); corrections from later passes are folded into the finding they affect.

---

## 2. High-severity issues (static analysis and verified)

### 2.5 `tss_async_signal_safe_thread_init` returns success when `create` yields NULL

`tss_async_signal_safe.c.ipp:209-217`: a `create` callback that returns 0 but leaves
`*dest` NULL makes `thread_init` return success (0) without inserting the TID into the
map, so a later `tss_async_signal_safe_get` on that thread returns NULL — the failure
is indistinguishable from success.

### 2.6 `tss_async_signal_safe_destroy(NULL)` dereferences NULL

The fallback `sig_global_tss_state_destroy` and user calls with a NULL/zeroed
handle crash: `tss_async_signal_safe_destroy(NULL)` -> `LOCK(mem->lock)` on NULL. There
is no validation of the handle in any of create/destroy/thread_init/get. **Extended by
Z8:** a second `destroy` on the same handle locks freed memory, and post-destroy
`get`/`thread_init` crash identically; **extended by X8:** `tss_async_signal_safe_create`
validates neither `val` nor `attr` (`attr == NULL` -> `memcpy` crash, `attr->create ==
NULL` crashes later in `thread_init`).

### 2.7 `sigguarded`/`sigfpe` NULL-argument handling aborts the process

Both backends `abort()` if `signals`, `guarded`, or `decider` is NULL
(`thrd_signal_handle_posix.c.ipp:249-253`, Windows `:255-259`). A library aborting on
argument errors is inconsistent with the rest of the API (which returns error codes) and
makes the failure mode a process crash. (The `sig_decision_invoke_recovery` decider
returning with a NULL `recovery` is legal per the docs; the abort-on-NULL applies only to
the three top-level arguments.)

### 2.8 `sigfence` on GNU compilers requires lvalues; non-lvalues fail to compile

`WG14_SIGNALS_SIGFENCE_IMPL_1(a)` expands to `__asm__ volatile(";" : "+m"(a) : : "memory")`.
The `+m` operand must be an lvalue: `sigfence(42)` or `sigfence(x + 1)` is a hard compile
error. Verified (`error: invalid lvalue in asm output`); the 0-arg form compiles. The
header now documents the requirement ("Any variable in the argument list MUST be a
lvalue", `thrd_signal_handle.h:248-249`), so the failure is documented if still a bare
compiler error.

### 2.9 AC4 [confirmed, Low] `sigfence` volatile-sink fallback (`DISABLE_INLINE_ASM`) rejects `volatile`/`const` arguments under `-Werror`

`thrd_signal_handle.h:193`: `WG14_SIGNALS_SIGFENCE_ESCAPE(a, i)` does
`sigfence_sink[(i)] = &(a)` — an implicit conversion from `volatile int *` (or `const
int *`, `volatile const int *`, etc.) to the `void *volatile` sink element, which
discards qualifiers. That is `-Wincompatible-pointer-types-discards-qualifiers`, an error
under the `-Werror` test build, whenever a qualifier-carrying lvalue is passed to
`sigfence` on a compiler using the volatile-sink fallback. This is exactly the Fil-C CI
leg: `cmake/filc-toolchain.cmake:32` defines `DISABLE_INLINE_ASM=1` (Fil-C cannot compile
the `+m` asm forms, analysis.md AA2), and `test/thrd_sigfpe_test.c:98` calls
`sigfence(result)` with `volatile int result` — the Fil-C job fails to build. Reproduced
with plain clang: `clang -Werror -DDISABLE_INLINE_ASM=1` on `sigfence(volatile_int)` gives
the same diagnostic. The asm path (`+m`) accepts qualified operands, so only the fallback
is affected.

**Fixed 2026-08-14:** the macro now casts explicitly: `sigfence_sink[(i)] = (void *)
&(a)`. The sink exists only to force the variable's address to escape into observable
memory, so the explicit qualifier-discarding cast is semantically correct. **Verified:**
`-DDISABLE_INLINE_ASM=1` builds of the full `ctest` suite (22 tests) pass on Linux
(clang 18, ASan/UBSan), including `sigfence_fence_test` whose volatile-sink escape check
(`sigfence_sink[0] != (void *) &a`) still passes, and `sigfence(volatile int / const int /
volatile const int)` combinations compile cleanly; the normal inline-asm path is
unchanged.

### 2.10 V2 [code-level, Windows] `win32_vectored_exception_function` NULL-derefs the per-thread state on fresh threads (High)

`thrd_signal_handle_windows.c.ipp:434-441`: when a global decider returns a claiming
decision, the handler calls `sig_global_tss_state()` and immediately dereferences
`tss->front`. The per-thread TLS state is created only by `sig_global_tss_state_init()`
(a prior `sigguarded`/`stdc_raise` on that thread); the vectored handler never
initialises it. A genuine fault (AV, div-by-zero) on a thread that has only ever called
`siginstall` (or nothing) -> `tss == NULL` -> NULL dereference *inside the exception
handler*, turning a recoverable fault into a crash. POSIX is immune (its handler inits
the TLS state as part of the raise). **The `sigguarded`-only sub-case (formerly X3) is
fixed since 2026-08-14:** `sigguarded` on Windows initialises the per-thread TSS like
POSIX, so this finding now covers only the thread whose *only* interaction is
`siginstall` (or nothing) — that thread's first genuine fault claimed by a global
decider still NULL-derefs `tss->front` in the vectored handler.

### 2.12 V4 [code-level, Windows] `stdc_raise` aborts for every unsupported signo (High)

`win32_exception_code_from_signal` (`thrd_signal_handle_windows.c.ipp:131-153`) handles
only SIGABRT/SIGBUS/SIGILL/SIGSEGV/SIGFPE;
`stdc_raise(SIGINT)`, `SIGTERM`, `SIGPIPE`, `SIGUSR1` etc. all hit `default: abort()`.
On POSIX the same calls are harmless no-ops when no decider is installed. The header
documents `stdc_raise` as usable for "OUR currently installed signal decider" for
arbitrary signals.

### 2.24 AB1 [confirmed, Medium-High] `signal_decider_destroy` leaks every decider node on the normal destroy path — the AA1 fix (65322dd) double-decrements the node refcount (regression) **[FIXED 2026-08-14]**

`thrd_signal_handle_common.ipp.ipp:708-755`. In `signal_decider_destroy`, for a signal
that is still installed (map entry present) whose node has the base refcount of 1 (the
normal `signal_decider_create` -> `signal_decider_destroy` cycle with no in-flight
raise):

1. the first decrement (`:712`) takes the refcount 1->0 and `LIST_REMOVE`s the node
   from the container's `global_handler` list;
2. the post-unlock "signal not installed" block (`:740-755`) then decrements **again**
   (`:749`): 0->-1, so `0 == -1` is false and `free(*retp)` never runs.

The node is left unlinked, unreferenced and leaked, with its refcount corrupted to -1.
**Verified:** with malloc/calloc interposition, 10,000 create->destroy cycles on one
installed signal leak 10,000 nodes (~400 KB) — every destroy observed `refcount_before=1`
in the first decrement and `refcount_before=0` (never freed) in the second. Before the
AA1 fix the post-unlock block was `free(*retp)` with no second decrement (the node was
freed exactly once); the AA1 fix changed it to `if(0 == --(*retp)->refcount) free(*retp)`
to defer the free for the concurrent map-miss case, silently breaking the still-installed
case. Invisible on the macOS CI legs (ASan on macOS has no LSan); the Linux CI legs would
catch it. The mid-loop `calloc`-failure path of `signal_decider_create` (`:642-648`,
which self-destroys the partial handle) leaks every node created so far via the same bug.

**Fixed 2026-08-14:** the refcount-zero branch now frees the node immediately — after the
`sighandler_info_has_decider` walk it does `free(*retp)` and sets `*retp = NULL`, so the
post-unlock block is only reached for the map-miss (uninstalled-signal) case. This is
safe because the raise path bumps the node's `refcount` under the same `state->lock`
*before* its unlocked decider call, so the map-entry refcount-zero branch can only be
entered when no in-flight raise references the node. **Verified:** full `ctest` suite (22
tests) passes under LeakSanitizer (ASan's `detect_leaks=1`, Linux arm64, clang 18) on all
four CI matrix combinations (native/fallback TLS x clang/gcc) and on the Release/C23/shared
combinations — previously 9 of 22 tests failed with `LeakSanitizer: detected memory
leaks` reporting 40-byte `calloc` from `signal_decider_create` (`:640`).

### 2.25 AC3 [code-level, fallback-TLS path, Low] `sig_global_tss_state_create` overwrites an existing TSS and leaks it

`thrd_signal_handle_common.ipp.ipp:342-359` (fallback path): the function calls
`tss_async_signal_safe_create()` which unconditionally assigns the fresh handle into the
static `*sig_tss_state_raw()` slot. If a TSS already exists there — recreated by the
documented post-uninstall setup call `stdc_raise(0, ...)`, or by an earlier `siginstall`
whose full `siguninstall` has not yet run — the old TSS is orphaned and leaked together
with every thread registration in it. The async-safe TLS path's create is a no-op and
initialises lazily, so the two paths diverge. Only visible under LSan on Linux with
`WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` (e.g. `post_uninstall_reentry_test` — 9
leaked `tss_async_signal_safe` handles + 9 verstable maps, ~1.9 KB per run).

**Fixed 2026-08-14:** the function now returns 0 (reuse) when the slot is already
non-NULL, matching the async-safe path's lazy-init semantics. **Verified:** the
`post_uninstall_reentry_test` / `stdc_raise_null_info_test` / `stdc_raise_uninstalled_test`
/ `install_consumer_test` LSan failures on the fallback path are gone; full `ctest` suite
(22 tests) passes under LSan on all matrix combinations.

---

## 3. Medium-severity issues

### 3.1 Spinlock is not async-signal-safe (deadlock risk in signal handlers)

`LOCK`/`UNLOCK` (`lock_unlock.h:33-61`) is a CAS spinlock with no signal masking, no
backoff, and no `pause`/`yield`. It is used inside signal-handler contexts on both
backends (`stdc_raise` -> `LOCK(state->lock)`, Windows vectored handler ->
`LOCK(state->lock)`, and the fallback `tss_async_signal_safe_get` -> `LOCK(mem->lock)`).
If a signal is delivered while the interrupted thread is itself holding the same lock,
the handler spins forever — a silent deadlock. The header's "usually async signal safe"
claim (`thrd_signal_handle.h:328-347`) does not cover re-entrancy.

### 3.2 First signal delivery on a fresh thread performs allocation inside the handler

`stdc_raise` -> `sig_global_tss_state_init` -> `calloc` + `thread_atexit` (which does
`std::vector` allocation / possibly throws) on the first call per thread
(`thrd_signal_handle_common.ipp.ipp:296-313`, `thread_atexit.cpp.ipp:71-85`). If the
first signal ever delivered to a thread arrives before any library call on that thread,
malloc and C++ heap operations run inside the handler (not async-signal-safe; risk of
deadlock on the heap lock). The docs recommend pre-calling `stdc_raise(0, ...)`; on Linux
this works, but the safety relies entirely on the user reading the docs.

### 3.3 `SA_NOCLDWAIT` + `SA_NODEFER` + missing `SA_RESTART` alter process semantics

`install_sighandler_impl` (`thrd_signal_handle_posix.c.ipp:400-412`) installs with
`sa_flags = SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER` for **every** signal:

- `SA_NOCLDWAIT` on `SIGCHLD` changes the process's child-reaping semantics: children are
  auto-reaped and `waitpid`/`wait` return `ECHILD`. `siginstall(NULL)` (install all
  signals — exactly what the tests do) silently breaks the host application's `waitpid`
  behaviour for the whole tenure of the install. It is only meaningful for SIGCHLD.
- `SA_NODEFER` permits re-entrant signal delivery; a fault in the handler re-enters it
  (infinite recursion / loops) — acknowledged in the docs, but dangerous.
- No `SA_RESTART`: syscalls interrupted by the installed signals return `EINTR` during
  the library's tenure even if the original handler had `SA_RESTART` (the old flags are
  discarded).

### 3.4 `invoke_sigaction` default handling is wrong for stop/continue signals and re-raises under `SA_NODEFER`

`thrd_signal_handle_posix.c.ipp:152-193`: the "default is to ignore" list only covers
SIGCHLD/SIGURG/SIGWINCH. Signals whose default action is "stop" (SIGSTOP, SIGTSTP,
SIGTTIN, SIGTTOU, SIGCONT) fall into the "reset to SIG_DFL and `pthread_kill(self)`"
branch. **Correction (C10):** re-raising with the handler reset to `SIG_DFL` permanently
discards the library's handler for stop/continue signals — after the process resumes from
a stop, the map still claims the signal installed while the kernel handler is now
`SIG_DFL`, so subsequent raises bypass the library until a re-install. **Correction
(C20):** the same applies to glibc realtime signals 34-64 installed by `siginstall(NULL)`
(Z6) — the documented `sigfillset_*` sets deliberately exclude them, yet the re-raise
path discards the library's handler for them.

### 3.5 `raw_signal_handler` on unknown signals silently installs SIG_DFL and re-raises

`thrd_signal_handle_posix.c.ipp:223-239`: if `stdc_raise` returns false, the handler
replaces itself with `SIG_DFL` and invokes `invoke_sigaction(&sa, ...)` where `sa` is the
freshly-minted SIG_DFL struct — for a default-ignore signal it returns false (no re-raise,
signal silently dropped); for others it re-raises as default. Reasonable, but the comment
admits "It shouldn't happen that this handler gets called when we have no knowledge of
the signal".

### 3.6 Thread-ID reuse with stale hash-table entries

`tss_async_signal_safe` maps are keyed by kernel thread ID (`current_thread_id`). If a
thread exits without running its atexit deinit (abnormal termination, `_exit` within a
thread is process-wide, cancellation corner cases, or `thread_atexit` registration
failing), the map retains the entry under that TID. A later thread that reuses the same
TID will observe the *previous* thread's value (never its own), and destruction may run
with stale state. There is no TID-generation counter.

### 3.7 `sig_global_state_tss_state_init` failure inside `stdc_raise` hides the real error

`stdc_raise` returns `false` both for "no handler installed for this signal" and for
"TSS init failed" (`thrd_signal_handle_posix.c.ipp:297-300`). The POSIX `signo == 0`
setup call also returns false on init failure, so the documented setup call gives no
diagnostic when setup actually failed.

### 3.8 Partial install failure in `siginstall` leaves handlers installed (no rollback)

`thrd_signal_handle_common.ipp.ipp:488-527`: if `install_sighandler` fails for any signal
in the set, `siginstall` frees the returned handle and returns NULL, but the signals
already installed remain installed and counted. The caller has no handle and no way to
uninstall them; a subsequent `siginstall` will double-count. `siguninstall` semantics
("threadsafe with respect to other concurrent executions of itself") also allow
concurrent partial uninstall while handlers are in flight.

### 3.9 `signal_decider_create` failure path partially self-destroys correctly but leaves warning-path signals uncounted

When `calloc` fails mid-loop, `signal_decider_create` calls `signal_decider_destroy(ret)`
on the partially-built handle, but `signal_decider_destroy` returns -1 (errno unchanged)
when it finds no matching slots — a misleading error signal. Additionally,
`WG14_SIGNALS_STDERR_PRINTF` runs while `state->lock` is held, which is slow and can
itself trigger a signal while the lock is held (see 3.1).

### 3.10 `signal_decider_destroy` frees nodes outside the lock

`thrd_signal_handle_common.ipp.ipp:736-757`: after decrementing a node's refcount to zero
under the lock and removing it from the list, `free(*retp)` runs after `UNLOCK`. In
practice the refcount increment precedes the unlocked decider call, so the current design
is safe — **confirmed by a fresh trace (C21):** the base refcount is 1 (create), an
in-flight raise increments to 2 *before* the unlocked decider call, and `destroy`'s
`--refcount` then takes the deferral branch; the raise's later `--refcount` reaches 0 and
moves the node to `deferred_frees`. The concurrent-destroy test genuinely exercises this
deferred path. Still, the free outside the lock is fragile and undocumented.

### 3.11 `thread_atexit` C++ exceptions disabled -> OOM terminates

`thread_atexit.cpp.ipp:71-85`: with `-fno-exceptions` the `try/catch` block is compiled
out; `std::vector::emplace_back` on allocation failure calls `std::terminate` instead of
returning -1. The library is designed to be embedded in C standard libraries where
exceptions may be disabled; this path then crashes instead of reporting failure.

### 3.12 Function-pointer type pun for atexit callback

`tss_async_signal_safe.c.ipp:246-248` casts `int (*)(struct deinit_state *)` to
`void (*)(void *)` and registers it via `thread_atexit`. Calling through an incompatible
function-pointer type is UB per the C standard (works on common ABIs, but a latent
portability hazard).

### 3.13 `tss_async_signal_safe_thread_init` re-entrancy (signal during `attr.create`) leaks

`tss_async_signal_safe.c.ipp:211-218` unlocks before calling the user's `create` and
re-locks before `insert`. If a signal handler runs `thread_init` on the same object in
that window (possible via `stdc_raise` -> `sig_global_tss_state_init` -> `thread_init`),
the user's `create` callback runs twice and the second `insert` replaces the first entry:
the first value leaks (no destructor on replace), `count` is double-incremented, and two
atexit registrations are queued.

### 3.14 `tss_async_signal_safe_thread_init` does not roll back on `thread_atexit` failure

`tss_async_signal_safe.c.ipp:228-249`: the map entry and `state->count` are committed
before `thread_atexit` is called; if it returns -1 the caller sees failure but the entry
and count remain, and no thread-exit cleanup will ever run for this thread. (On the
async-safe TLS path, `sig_global_tss_state_init` has the same pattern — it sets `*state =
mem` *before* `thread_atexit(free, mem)`; on registration failure the pointer stays set
and the next call silently succeeds with a leaked `mem`, M2.)

### 3.15 V5 [code-level, Windows, Medium] Global deciders can be invoked two or three times per single exception

The same function is registered both as `AddVectoredContinueHandler` and as the
unhandled exception filter (`install_sighandler_impl`, `:486-505`). **Correction (C15/C16):**
with no `AddVectoredExceptionHandler`, the effective dispatch order is frames-first:
frame `__except` filters (frame deciders) -> unhandled filter (global deciders) ->
continue handler (global deciders again). The library function runs at most **twice** per
exception, and only **once** under a debugger (the unhandled filter is not invoked under
a debugger). So the side-effecting-decider double-run claim stands for the no-debugger
path when no decider claims; the pass-5 debate over exactly one vs two invocations on the
no-debugger path (C19) was left unresolved — the Windows CI's
`thrd_signal_handle_test` asserting `count_decider == 1` for a *claiming* decider passes,
consistent with the claiming path running once. **2026-08-14 data point (C19 resolved for
the CONTINUE_EXECUTION resolution):** when the claiming decider is resolved via
`EXCEPTION_CONTINUE_EXECUTION` rather than a `longjmp` out of dispatch (no `stdc_raise`
frame, e.g. a genuine/software fault inside a `sigguarded` frame), the vectored continue
handler runs the global decider a *second* time on the no-debugger path — the
`sigguarded_tss_init_test` Windows CI run observed `global_decider_called == 2`. The
`longjmp` resolution aborts the dispatch after one claim, which is why
`thrd_signal_handle_test` sees exactly one.

### 3.16 V7 [code-level, fallback path, Medium] `siguninstall` of the last handler while another thread is inside `sigguarded` frees that thread's live state

On the fallback (Apple) path, the final `siguninstall` -> `sig_global_tss_state_destroy`
-> `tss_async_signal_safe_destroy` frees the shared TSS (and every per-thread entry)
*while another thread may be between `sigguarded` frames*. The interrupted thread's
`tss->front` points into the freed per-thread state; its next raise runs
`sig_global_tss_state_init` against the *dangling* `*sig_tss_state_raw()` -> UAF.
Only the fallback path is affected (the async-safe TLS path's destroy is a no-op). The
usage requirement "destroy only after all threads have left `sigguarded`" is nowhere
documented.

### 3.17 W3 [code-level, POSIX, Medium] nested signal delivery during a decider call races on the shared frame `rsi`

`thrd_signal_handle_posix.c.ipp:312-315` — the frame's `rsi` is both written
(`prepare_rsi`) and read (`frame->decider(&frame->rsi)`) from the same thread. With
`SA_NODEFER`, a second delivery of a guarded signal while the first decider is still
executing re-enters `raw_signal_handler` -> `stdc_raise` -> `prepare_rsi` on the **same**
`frame->rsi`, overwriting the fields mid-decider: the outer decider reads torn fields, and
if the nested raise chooses `invoke_recovery` it `longjmp`s out of the outer decider —
the *outer* recovery runs with the *nested* `rsi` contents. Not a C memory-model race
(same thread), but a re-entrancy aliasing bug; on Windows the frame `rsi` is a fresh
local in `win32_exception_filter`, so nested exceptions cannot corrupt it.

### 3.18 X4 [code-level, Windows, Medium] `EXCEPTION_STACK_OVERFLOW` (0xC00000FD) is not mapped to any signal

`signal_from_win32_exception_code` (`thrd_signal_handle_windows.c.ipp:154-183`) covers
the five raised signals plus the eight `EXCEPTION_FLT_*`/`EXCEPTION_INT_*` codes, but has
no case for `EXCEPTION_STACK_OVERFLOW`. A genuine stack overflow returns `signo == 0` ->
`EXCEPTION_CONTINUE_SEARCH` -> WER terminates the process with no library involvement,
whereas on POSIX a stack overflow delivers `SIGSEGV`, which *is* handled. A real
functional gap between backends for a common, otherwise-recoverable fault class, and it
is not documented.

### 3.19 Z2 [confirmed, Low-Medium] `stdc_raise(signo, NULL, NULL)` hands off to a pre-existing `SA_SIGINFO` handler with NULL `siginfo_t *` and NULL `ucontext_t *`

`thrd_signal_handle_posix.c.ipp:396`: when no frame or global decider claims the
raise and the pre-library handler was installed with `SA_SIGINFO`, `invoke_sigaction`
calls `sa->sa_sigaction(signo, NULL, NULL)` — the NULL `info`/`raw_context` are passed
through unchanged. Verified: `stdc_raise(SIGUSR1, NULL, NULL)` invokes the handler with
`si == 0x0` (a real `raise()` delivers a valid `si`). Any application handler that reads
`si->si_signo`/`si->si_addr` crashes. The library's own `raw_signal_handler` always passes
the kernel's real `siginfo`, so this is specific to the documented "pass on signal
handling to this library" API. The header does not warn that the previous handler may
receive NULL pointers. Fix direction: synthesise a minimal `siginfo_t` when `info ==
NULL`, or document the NULL-pointer hand-off.

### 3.20 Z9 [code-level, Low] `thread_init`'s unlocked `attr.create` breaks the THREADSAFE contract for concurrent first-use on distinct threads

`tss_async_signal_safe.c.ipp:211-218`: the user's `create` callback runs **without**
`mem->lock`, so two threads racing to first-initialise the same handle call the user's
`create` concurrently. The API documents `thread_init` as THREADSAFE and serialises the
map insert but not the create callback. The test suite's own `create` uses a shared
`static unsigned *storage_ptr`, so a two-worker concurrent first-use would be a data race
in the harness itself. Design note: either document that `create` must be thread-safe, or
serialise the create callback under the lock (at the cost of re-entrancy, cf. 3.13).

### 3.21 AC2 [code-level, Low] the C `thread_atexit` silently swallows a failed `__cxa_thread_atexit()` registration

`thread_atexit.c.ipp:66-76` — the C implementation (used whenever
`__cxa_thread_atexit()` is available, i.e. the glibc/macOS/FreeBSD builds) calls
`__cxa_thread_atexit(func, obj, &thread_atexit_dso_symbol)` and unconditionally
`return 0`, ignoring the return value. The comment justifies this because macOS's
`__cxa_thread_atexit` return is unreliable, but on glibc it is the reliable
`__cxa_thread_atexit_impl` wrapper (libsupc++/libc++) that genuinely returns -1 on
ENOMEM. A dropped registration means the registered deinit never runs at thread exit:

1. `tss_async_signal_safe_thread_init` returns success (its `res = thread_atexit(...)`
   is always 0) while no thread-exit cleanup is scheduled — the per-thread map entry and
   the shared `deinit_state` leak, exactly the state 3.14/M2 describe, but with no
   failure ever surfaced to the caller (the 3.14 rollback is unreachable on this path).
2. `sig_global_tss_state_init` (async-safe TLS path,
   `thrd_signal_handle_common.ipp.ipp:296-313`) sets `*state = mem` before calling
   `thread_atexit(free, mem)`; an invisible registration failure leaves `*state` set and
   the `calloc`'d state leaked at thread exit.

Fix direction: probe at configure time whether the platform's `__cxa_thread_atexit`
return is reliable (the CMake `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` probe already
exists) and propagate the return on platforms where it is, keeping the ignore-everything
behaviour only for the known-unreliable ones (macOS).

---

## 4. Portability and configuration concerns

### 4.2 `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` auto-detection is too optimistic

`config.h:51-61` enables async-safe TLS for *any* `__GNUC__` (which includes clang) on
any non-Apple platform. This is only true where the toolchain actually supports
`tls_model("initial-exec")` and the libc reserves static TLS for dlopened libraries.
Also, a user-defined `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on a compiler that is
neither `__GNUC__` nor `_MSC_VER` leaves `WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL` undefined
while code references it -> compile error with no diagnostic.

### 4.3 `pthread_getthreadid_np` / `mach_thread_self` are not async-signal-safe

`current_thread_id.c.ipp:80-83`: the Apple branch performs `mach_port_deallocate` (kernel
round-trip) on every cache miss; `current_thread_id()` is documented as "ASYNC SIGNAL
SAFE" (`current_thread_id.h:64-65`). On fallback platforms the cache miss happens inside a
signal handler on first use -> async-signal-unsafe syscalls.

### 4.4 MSVC CRT signals bypass SEH (Windows)

`siginstall` on Windows installs only SEH vectored handlers
(`thrd_signal_handle_windows.c.ipp:486-505`); it does not install CRT signal handlers.
`abort()`, `raise(SIGFPE)`, `assert`, etc. on MSVC dispatch through the CRT, which does
not raise SEH exceptions — so they never reach the library. Only genuinely SEH-raised
exceptions (access violations, integer overflow traps on x86, explicit `RaiseException`)
are handled. A substantial functional gap on Windows versus POSIX, and it is not
documented.

### 4.6 Missing `SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards

`thrd_signal_handle_posix.c.ipp:53-54` uses `SIGSYS`, and `:109` uses `SIGXCPU`/`SIGXFSZ`
without `#ifdef` guards (only `SIGPOLL` is guarded). On a POSIX platform that omits any of
these the file fails to compile. The library build now compiles with explicit feature-test
macros (done 2026-08-14, formerly ideas.md 2.2), so glibc/musl consistently expose these
signals; the missing `#ifdef` guards remain for platforms that omit the signals entirely.

### 4.7 `ucontext_t` and `siginfo_t` portability

`thrd_signal_handle.h:202-216` relies on `<signal.h>` defining `ucontext_t` (POSIX does
not require this; `<ucontext.h>` does) and uses platform-specific spellings
(`struct __siginfo`, `struct siginfo`) that assume BSD/Android/glibc layouts.

### 4.8 Mingw

Deliberately unsupported (`#error` at `thrd_signal_handle_windows.c.ipp:273-275`), but
before reaching that `#error` the header has already redefined `sigset_t` on `_WIN32`
(`thrd_signal_handle.h:43`), which collides with MinGW's own `sigset_t` typedef — the
first of several Mingw incompatibilities.

### 4.9 `_setjmp` vs `setjmp` inconsistency for header-only consumers

`WG14_SIGNALS_HAVE__SETJMP` is set only on the compiled library target
(`CMakeLists.txt:176-178`, PRIVATE). Header-only consumers never get the definition and
always use `setjmp` (saving/restoring the signal mask) even when `_setjmp` is available.
Not a correctness bug but a silent performance/behaviour split between the two modes.

### 4.10 `__VA_OPT__` dependency **[FIXED 2026-08-14]**

`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:86`) requires `__VA_OPT__`
(C23 / C++20, or the GCC>=8 / Clang>=12 extension). Verified: GCC and Clang accept it
silently in C11 mode with `-Wpedantic`, so this works on current toolchains, but it is a
latent portability break for older compilers or strict MSVC C modes.

**Fixed 2026-08-14:** the Windows CI (VS2022) failed the C++ header-only test
(`header_only_test`) with `error C2338: static assertion failed: ... sigfence()
requires __VA_OPT__ argument counting`: MSVC only enables `__VA_OPT__` by default for
`/std:c11`, `/std:c17` and `/std:c++20+` (VS 2022 17.9+); C++14/17 needs the explicit
`/Zc:__VAOPT__` opt-in (available since VS 2019 16.10). The C test targets pass because
they compile with `c_std_11` (`/std:c11`). Fixed by adding `/Zc:__VAOPT__` to every MSVC
target's compile options: the library target (`CMakeLists.txt`, PUBLIC so `find_package`
consumers inherit it), `add_code_example` (all C test targets), `header_only_test` and
`header_only_c_multi_test` (`test/CMakeLists.txt`), the install consumer
(`test/install_consumer/CMakeLists.txt`) and the header-only C consumer
(`test/header_only_c_consumer/CMakeLists.txt`). **Verified:** clang-cl accepts
`/Zc:__VAOPT__` as a no-op (harmless for non-MSVC), GCC/Clang on Linux and macOS are
unaffected by the MSVC-only flag and the full 22-test `ctest` suite passes on Linux
(native + fallback TLS) and macOS.

### 4.11 X10 [code-level, Medium-Low] musl (Alpine etc.) fails to compile: wrong `siginfo_t` fallback spelling

`thrd_signal_handle.h:202-208` — the platform dispatch is `_WIN32` / `__GLIBC__` /
`__ANDROID__` / `else: struct __siginfo`. musl defines `siginfo_t` as `struct siginfo`,
not `struct __siginfo`, and defines neither `__GLIBC__` nor `__ANDROID__` — so musl
builds fall into the `struct __siginfo` branch and fail to compile. The CI matrix only
exercises glibc; musl-based Linux breaks at compile time.

### 4.12 Y9 [verified probe, Low, refines 4.2] forcing `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles and links — silently unsafe

Pass 1's 4.2 predicted a compile error for a user-forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1`
on Apple. The probe shows clang on macOS *accepts* `__attribute__((tls_model("initial-exec")))`
on Mach-O (parsed and ignored/approximated), so the build succeeds and the real
consequence is silent: Mach-O TLS accesses (`tlv_get_addr` machinery) are not
async-signal-safe, so the library quietly violates its own "ASYNC SIGNAL SAFE" contract —
including inside `current_thread_id()`/`tss_async_signal_safe_get()` called from
handlers. The failure mode is a silent safety regression, not a diagnostic. (Same class
as the 4.2 note for non-GNU compilers, but on Apple the user is not warned at all.)

### 4.13 Y5 [code-level, Low] no `pthread_atfork` handling; stale TID caches are inherited across `fork()`

There are no `pthread_atfork` registrations anywhere in the library. After `fork()` in a
multi-threaded process, the child inherits `current_thread_id_cached` (initial-exec TLS,
`current_thread_id.c.ipp:64-70`) and `my_current_thread_id`
(`tss_async_signal_safe.c.ipp:84-94`) holding the *parent's* TID — every
`current_thread_id()`/`tss_async_signal_safe_get()` in the child returns the wrong
identity for the child's lifetime — plus the copied `thread_id_to_tls_map` and
`sig_global_state`, so the child's map lookups keyed by the stale parent TID can return
the *parent's* per-thread values. For a library whose stated purpose is thread-local
signal handling inside a C runtime, fork-safety is a realistic requirement and is neither
implemented nor documented.

### 4.14 Y6 [code-level, Low] `NSIG` is not POSIX-mandated; a missing `NSIG` silently disables `siginstall`

`thrd_signal_handle_common.ipp.ipp:61-62` uses `#if NSIG < 1024` (undefined `NSIG`
evaluates to 0), and the `siginstall`/`siguninstall`/decider loops iterate `1 .. NSIG-1` —
with `NSIG` undefined the loops never execute and `siginstall` **returns success having
installed nothing**. All CI platforms define NSIG, so this is exotic-POSIX-only, but the
failure is silent.

### 4.15 Z4 [code-level, Low, extends X7/V4] negative `signo` in `stdc_raise` is UB on the POSIX frame walk and `abort()`s on Windows

`thrd_signal_handle_posix.c.ipp:312`: `sigismember(frame->guarded, signo)` with a negative
`signo` expands (macOS/BSD macro form) to `1u << (signo - 1)` — a negative/oversized shift
count, i.e. UB, whenever any frame exists. On Windows, `stdc_raise(-1, ...)` ->
`win32_exception_code_from_signal` `default: abort()`. Three platforms give three
different behaviours for the same invalid input.

### 4.16 X7 [code-level, Low] out-of-range `signo` reaches `sigismember` without bounds checks (UB on BSD/macOS)

`thrd_signal_handle_posix.c.ipp:310-336` — the frame loop tests
`sigismember(frame->guarded, signo)` with no `signo < NSIG` check. On macOS/BSD
`sigismember` is a macro expanding to `(*(set) & (1u << (signo - 1)))` (verified via
`-dM`); `stdc_raise(64, ...)` is a shift-count UB. On glibc, `sigismember` is a function
that returns 0 for out-of-range values, so the bug is invisible there. The same call on
Windows either `abort()`s (V4) or silently no-ops — three different behaviours for the
same invalid input across platforms.

### 4.17 Z6 [code-level, Linux/glibc, Low] `siginstall(NULL)` installs handlers for glibc-internal signals 32/33 and realtime signals 34-64

On glibc `NSIG == 65`, so `siginstall(NULL)` (the pattern used by every test and the
README) installs the library's handler for signals 32 (`SIGCANCEL`) and 33 (`SIGSETXID`,
glibc's internal pthread-cancellation/setxid signals) and for all 31 realtime signals
34-64. `SIGCANCEL`/`SIGSETXID`: the library *replaces* glibc's internal handler and
chains to it on each delivery, adding latency to every pthread cancellation, allowing a
decider to swallow cancellation, and `SA_NODEFER` changes glibc's expected blocking
semantics. Realtime signals: the default path in `invoke_sigaction` resets to `SIG_DFL`
and re-raises, so a realtime-signal delivery routes through the library and its default
re-raise discards the library handler permanently (the 3.4/C10 family), even though
`sigfillset_synchronous`/`_asynchronous_*` deliberately do not include realtime signals.
The internal and realtime ranges are neither skipped nor documented.

---

## 5. Build-system and packaging issues

### 5.2 Static library requires a C++ runtime, not declared [open on non-`__cxa_thread_atexit()` platforms]

On platforms without `__cxa_thread_atexit()` (e.g. Windows), `thread_atexit.cpp` is
compiled into the library (C++) and the CMake package does not express the C++ standard
library dependency, so a plain C consumer linking `libwg14_signals.a` there gets
unresolved C++ runtime symbols. (Fixed 2026-08-10 on `__cxa_thread_atexit()` platforms:
`src/wg14_signals/thread_atexit.c` is compiled instead and the C++ file is neither
compiled nor linked — the library is all-C with no C++ runtime dependency there.) The
open remainder is the Windows/non-`__cxa_thread_atexit()` case.

### 5.3 CMake `CMAKE_C_STANDARD` cache variable is unused for consumers **[PARTIALLY FIXED 2026-08-14]**

The `CMAKE_C_STANDARD` cache variable set at `CMakeLists.txt:6` is not propagated to
consumers of the installed package; the `find_package` consumer must re-declare its own
`CMAKE_C_STANDARD` (the install consumer does, at `test/install_consumer/CMakeLists.txt:10-11`).
Fixed 2026-08-14: the tests, `header_only_test`, `header_only_c_multi_test` and the
install-consumer all now build with `-Wall -Wextra -Wpedantic -Werror` (done 2026-08-14,
formerly ideas.md 2.5), so test-only warnings surface. Still open: the `CMAKE_C_STANDARD`
cache variable is not propagated to consumers of the installed package.

### 5.4 CI gaps

- The Windows CI runs with `-DCMAKE_C_STANDARD` in {11,17} but MSVC ignores the C-standard
  option for `/experimental:c11atomics` in some versions.
- No CI runs the benchmark targets at all (`-E benchmark`), so the performance claims in
  the README are not verified.
- `header_only_build_test`'s single-TU C header-only consumer fails on FreeBSD —
  `current_thread_id()` returns 0 there, while the library build, the C++ and C multi-TU
  header-only builds, and `header_only_c_multi_test` all return a non-zero tid; the
  single-TU weak `_Thread_local` `current_thread_id_cached` retention is suspected. The
  test is excluded from the FreeBSD ctest run pending diagnosis; the consumer now prints
  which check failed to make the next run conclusive. (Fixed 2026-08-14: the Linux and
  MacOS CI jobs gained the `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS` OFF/ON matrix dimension
  (done, formerly ideas.md 2.1/3.4), so the `tss_async_signal_safe` hash-table fallback now
  runs under ASan/UBSan on Linux — the platform with the strongest tooling.)

### 5.5 `ProjectConfig.cmake.in` references non-existent export names

`cmake/ProjectConfig.cmake.in:6-11` conditionally includes
`@PROJECT_NAME@SlExports.cmake`/`@PROJECT_NAME@DlExports.cmake`, which are never
generated. Harmless (guarded by `EXISTS`), but misleading.

### 5.7 W6 [build, Medium] `PROJECT_IS_TOP_LEVEL` requires CMake >= 3.21 while `cmake_minimum_required` is 3.15

`CMakeLists.txt:1` declares `cmake_minimum_required(VERSION 3.15 FATAL_ERROR)`, but line
231 gates the entire `test/` subdirectory on `PROJECT_IS_TOP_LEVEL`, a variable introduced
in CMake 3.21. On CMake 3.15-3.20 the variable is undefined and the condition silently
evaluates false: no tests, no `header_only_test` target, and `BUILD_TESTING` is ignored —
with no diagnostic. Either raise the minimum to 3.21 or use
`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`.

### 5.8 W7 [build, minor] MSVC builds lack the `-Werror` equivalent

`CMakeLists.txt:188-192`: GCC/Clang get `-Wall -Wextra -Wpedantic -Werror`; MSVC gets
`/W4 /experimental:c11atomics` with no `/WX`. All warnings that would break a strict
GCC/Clang build are invisible in the Windows CI leg (extends 5.3).

### 5.9 Y7 [build, Low] the project unconditionally requires a C++ compiler for a C library

`CMakeLists.txt:19` declares `project(wg14_signals LANGUAGES C CXX)` unconditionally (only
the *chosen* `thread_atexit` source is conditional, at `:80-84`), so a C-only toolchain
cannot configure the project on any platform — even on `__cxa_thread_atexit()` platforms
where the compiled library is all-C.

---

## 6. Test-quality issues

### 6.1 `WG14_SIGNALS_PREFIX(fn(args))` misuse (cosmetic only — C11)

`test/thrd_signal_handle_test.c:101,121`, `test/thrd_sigfpe_test.c:111`,
`test/benchmark_thrd_signal_handle_test.c:116` all write
`WG14_SIGNALS_PREFIX(signal_decider_destroy(sigill_decider))` — the prefix macro is
applied to the whole call expression. **Correction (C11, refutes the original claim):**
this does *not* break custom-prefix builds — the macro argument contains no prefixable
identifiers, so the expansion is exactly the correct call. Verified: the library builds
with `#define WG14_SIGNALS_PREFIX(x) foo_##x` (function-like) and all four C test
programs compile. The spelling is still misleading and should be fixed for clarity.
**Note (C17):** the object-like spelling `-DWG14_SIGNALS_PREFIX=foo_` does *not* work —
the macro must be function-like, which is a documentation gap in `config.h`, not a
library defect.

### 6.2 Test storage exhaustion

`test/async_signal_safe_tls_test.c:6-14` and `test/header_only_test.cpp:16-24`: `create`
does `*dest = storage_ptr++` on a 2-element array; any third `thread_init` (e.g., a
re-run of the main-thread init after the worker also inits, or the documented "safe to
call many times") writes out of bounds. **Extended (C7):** the same applies to
`test/benchmark_async_signal_safe_tls_test.c:12-13`. The test also never verifies the
re-init and re-entrancy semantics documented in the API.

### 6.3 No coverage of the failure/edge APIs

No test calls `siguninstall_system`, `sigfillset_synchronous/asynchronous_*` return
values, or `signal_decider_create` with NULL/empty guarded sets.

### 6.4 The SIGFPE test depends on architecture trap behaviour

`test/thrd_sigfpe_test.c:62-67` works around x64's lack of integer-divide trapping, but
the guard relies on `sigfence` and `stdc_raise(SIGFPE)` fallback, so the "real fault" SEH
path on Windows is never exercised.

### 6.5 X12 [test-harness, Low] `test_common.h` `thrd_join`/`thrd_create` defects

`test_common.h:81-100` — `thrd_join` checks `ret != -1`, but `pthread_join` returns an
error *number* (0 on success), never -1: on failure `*res` is left unset while the caller
proceeds as if the join succeeded. `thrd_create` (`:81-89`) dereferences the unchecked
`calloc` result (NULL deref on OOM). The benchmark and handle tests rely on this shim; the
harness masks real failures.

---

## 7. Async-signal-safety claims vs reality

The public API makes strong claims ("ASYNC-SIGNAL-SAFE", "USUALLY ASYNC-SIGNAL-SAFE").
Verdicts:

- `tss_async_signal_safe_get`: NOT safe in the general case. It takes a spinlock (3.1),
  reads a cached thread ID (fine once populated), and on cache-miss performs
  async-signal-unsafe syscalls on Apple (4.3). Safe only when (a) the TLS cache was
  primed outside the handler and (b) no other thread/handler holds the object's lock.
- `current_thread_id`: safe on Linux/ELF (initial-exec TLS; the `gettid` syscall is
  signal-safe) and Windows; unsafe on Apple on first use per thread (4.3).
- `sigfillset_*`: safe (read-only static init with a benign double-checked write race —
  see 7.1).
- `sigguarded` / `stdc_raise`: "usually safe" only after the per-thread setup call
  (`stdc_raise(0, ...)`). On Linux with the documented pre-call, the happy path is
  signal-safe.
- `siginstall` / `siguninstall` / `signal_decider_create` / `signal_decider_destroy`:
  NOT async-signal-safe (malloc, fprintf, locks) — correctly documented as
  THREADSAFE-only.

### 7.1 Data race in the `sigfillset_*` lazy initialisation

`thrd_signal_handle_posix.c.ipp:58-76` double-checks `sigismember(&v, signos[0])` then
writes `v = x` without a lock or atomics. Two threads calling `sigfillset_synchronous`
concurrently both compute `x` and both store `v` — a benign write-write race on identical
values, but still a data race under the C memory model (UB), and the "is it initialised?"
check reads a non-atomic while another thread writes. In practice the sigset write is
aligned and atomic; the sets also carry `__attribute__((constructor))` (M4), which
pre-initialises the statics at load time for executables and substantially mitigates the
race — but the attribute is POSIX-only (C9), so the race is unmitigated on Windows for
`synchronous_sigset`/`asynchronous_nondebug_sigset`.

### 7.3 AA8 [code-level, Low] the `tss_async_signal_safe` per-thread ID cache uses plain `_Thread_local`, not the async-signal-safe attribute

`tss_async_signal_safe.c.ipp:84-94` declares the `my_current_thread_id()` cache with
`WG14_SIGNALS_THREAD_LOCAL` (plain `_Thread_local`), not
`WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL`. On platforms where the library already has
async-signal-safe TLS available (Linux/Windows), this cache is still global-dynamic TLS on
ELF, whose first access calls `__tls_get_addr` — not async-signal-safe. The cache is
normally primed by the documented `thread_init` call, so `tss_async_signal_safe_get`
(documented ASYNC-SIGNAL-SAFE) is only actually safe after priming; the first `get` from a
handler on a never-primed thread is async-signal-unsafe even on Linux. Extends the 4.3/Y9
family to the async-safe path's own cache.

---

## 8. setjmp/longjmp correctness concerns

### 8.1 Modified-local-after-setjmp UB in POSIX `sigguarded`

`thrd_signal_handle_posix.c.ipp:241-287`: `current.rsi` is written by `prepare_rsi` (via
the frame pointer in the signal handler, i.e. after `setjmp` executed) and then read after
`longjmp`. Per C11 7.13.2.1p3, non-volatile automatic objects modified between `setjmp`
and `longjmp` have indeterminate values after `longjmp` — this is UB (works in practice on
mainstream compilers because the frame is a memory object, but a conforming compiler may
cache `current` in registers). The struct should be `volatile` (or the members accessed
post-longjmp should be).

### 8.2 `_setjmp`/`setjmp` selection changes signal-mask semantics

With `setjmp` (used when `_setjmp` is unavailable) the mask saved at the `setjmp` is
restored on `longjmp` — combined with `SA_NODEFER` handlers this can silently
unblock/block signals relative to the interrupted context. Platform-dependent.

### 8.3 Y2 [confirmed, POSIX, Medium] user `longjmp` out of `guarded()` leaves `tss->front` pointing at a dead frame

`thrd_signal_handle_posix.c.ipp:249-287`: `sigguarded` pushes `current` onto `tss->front`
and pops it on the two normal exits. Neither the Windows backend (which never pushes) nor
the docs forbid the guarded function from using `setjmp`/`longjmp` for its own error
handling; a `longjmp` out of `guarded()` to a caller frame **above** `sigguarded` bypasses
both pop sites. The next `stdc_raise` on that thread walks `frame->guarded`/`decider`/`buf`
in freed stack — verified ASan `stack-use-after-scope` at `thrd_signal_handle_posix.c.ipp:310`;
a real signal delivered to that thread does the same *inside the handler*. On the
async-safe TLS path the stale frame is never cleared, so the corruption persists
indefinitely. Related sub-race (Y2b, code-level): a raise delivered after `guarded()`
returns but before `tss->front = old` executes still finds the frame and, if it chooses
`invoke_recovery`, silently discards `guarded()`'s return value — a few-instruction window
in the same family as V6 (below).

### 8.4 V6 [code-level, both backends, Medium] Frame published before `setjmp` completes -> longjmp into an uninitialised buffer

POSIX `sigguarded` (`thrd_signal_handle_posix.c.ipp:271-272`) and Windows `stdc_raise`
(`thrd_signal_handle_windows.c.ipp:315-316`) both execute `tss->front = &current` before
`setjmp(current.buf)` returns. A signal/exception delivered in that instruction window
runs `stdc_raise`/the vectored handler, which may `longjmp(current.buf)` before `setjmp`
has stored the environment — undefined behaviour. The window is a few instructions wide but
is exactly the async nature the API claims to handle.

### 8.5 W9 [C++ consumers, Low] longjmp across objects with non-trivial destructors is UB

`stdc_raise`'s `invoke_recovery` path (`thrd_signal_handle_posix.c.ipp:332`) and the
Windows vectored handler's `longjmp` (`thrd_signal_handle_windows.c.ipp:440`) skip C++
destructors for any automatic object live in the guarded frame — UB per the C++ standard.
The library is documented as C++-usable, and MSVC explicitly disables warning 4611
(`thrd_signal_handle_windows.c.ipp:46`) for it. A C++ `sigguarded` caller with RAII objects
in scope of a raised signal gets skipped destructors silently.

### 8.6 Global deciders may never return (e.g. `siglongjmp` elsewhere) -> leaked node/container reference counts [code-level, both backends, Medium]

A decider function is only restricted to async-signal-safe calls
(`thrd_signal_handle.h:522-524`); `siglongjmp` and `_exit` are on the POSIX
async-signal-safe list, and nothing in the docs forbids a decider from transferring
control away and never returning. The library's own frame-recovery decision already
transfers control non-locally (`sig_decision_invoke_recovery` longjmps to the guarded
frame's `jmp_buf`). In the *global* decider path, however, each invocation increments the
decider node's `refcount` and the container's `lifetime_refcount` before the unlocked
decider call (`thrd_signal_handle_posix.c.ipp:358-366`; Windows vectored handler
`thrd_signal_handle_windows.c.ipp:404-416`), and the matching decrements
(`--current->refcount`, `sighandler_info_release`) only run *after* the decider returns.
A decider that never returns (e.g. `siglongjmp` to a caller-owned buffer, or a
non-terminating loop) therefore abandons the raise with both references held forever:

1. The node `refcount` stays elevated, so the node stays linked in
   `global_handler` and `signal_decider_destroy` can never retire it (its `--refcount`
   never reaches zero; the handle slot is merely nulled) — a leak that grows
   unboundedly if the user keeps creating deciders after each abandoned raise.
2. The container `lifetime_refcount` stays >= 1, so `siguninstall` dropping the map's
   reference never brings it to zero and `sighandler_info_release` never frees the
   container — the container and every node on its `global_handler`/`deferred_frees`
   lists leak permanently (the `state->lock` is *not* held across the decider call, so
   there is no spinlock leak, only memory).

`_exit()` inside a decider is harmless (process teardown reclaims all memory). A
never-returning *frame* decider is the Y2/Y2b family (`tss->front` left pointing at a
dead frame, 8.3). Severity Medium: no UAF/crash, but unbounded leak per abandoned raise.

**Future handling.** The implementation must eventually cope with non-returning deciders.
Options, in increasing robustness: (a) document that deciders must return and treat a
never-returning decider as unsupported; (b) make the reference release the *thread's*
responsibility — record the abandoned container/node in per-thread raise state and drain
it on the next library entry or at thread exit (via the existing `thread_atexit` hook),
bounding the leak to one outstanding abandoned raise per thread; (c) hold the release on a
per-thread "in-flight raise" guard pushed before each unlocked decider call and popped on
return, with the same drain-on-entry/exit.

---

## 9. Minor issues and observations

- `siguninstall`'s `-1` failure path (`thrd_signal_handle_common.ipp.ipp:545-548`) leaks
  `ss`; and since `uninstall_sighandler` always returns `true`, the error path is dead
  code.
- `signal_decider_destroy` acquires `state->lock` per signal (NSIG iterations), even for
  signals not in the guarded set — O(NSIG) lock round-trips for a single destroy.
- `install_sighandler` never flushes `deferred_frees` (only uninstall/destroy do), so
  deferred nodes are held until an uninstall — a minor memory retention.
- `siginstall` returns the allocated `sigset_t *`; there is no way to uninstall by signal
  subset, and `siguninstall` frees the passed pointer, making double-uninstall a UAF
  (user error, undocumented). **Extended (M1):** the docs never state that the pointer
  must be the exact value returned by `siginstall`; passing a user-owned sigset (stack
  object) -> invalid free.
- The `benchmark_thrd_signal_handle_test.c` uses `CHECK()` inside the timed loop (line
  103), which adds `fprintf` overhead to the measured critical path on failure only —
  negligible but non-idiomatic.
- `config.h:137-145` opens and closes an empty `extern "C"` block — harmless but dead.
- `Readme.md` "Known issues and limitations" lists the Fil-C `sigaction` limitation, the
  global-decider `invoke_recovery` divergence, and the `pcpp` future work, but none of
  the open findings above; the plan files remain the only inventory.
- `doc/html/` is a committed Doxygen build output — version-controlled generated artifacts
  (churn, but not a bug).
- **AC1 [docs-hygiene, Low]** many source comments and the CI YAML cite finding IDs that
  were removed by the 2026-08-14 purges: `analysis.md 5.10` (Fil-C/FreeBSD, cited by
  `test/thrd_sigfpe_test.c:6`, `test/recovery_null_loop_test.c:6`,
  `test/post_uninstall_reentry_test.c:21`, `test/header_only_build_test.cmake:17`,
  `ci.yml:332`), `analysis.md 2.9`/`W11` (sigfence codegen, cited by
  `test/sigfence_fence_test.c:1`, `test/sigfence_codegen_test.cmake:1`), and `analysis.md
  1.8`/`C3`/`Y10` (header-only, cited by `test/header_only_build_test.cmake` and
  `test/header_only_c_consumer/main.c`). The IDs no longer resolve in `plans/analysis.md`;
  retarget the comments to the surviving IDs (e.g. the Fil-C/FreeBSD exclusions are now
  documented only in `ci.yml` itself).

### 9.1 M3 `sigguarded` failure return is indistinguishable from a legitimate -1

`sigguarded` failure returns `ret.int_value = -1`, which is indistinguishable from a
guarded function legitimately returning -1; no error return is documented for `sigguarded`.

### 9.2 V8 [code-level, Windows, Low] `stdc_raise(SIGABRT)` raises a non-continuable exception; "resume" from a decider loops

`SIGABRT` maps to `EXCEPTION_NONCONTINUABLE_EXCEPTION (0xC0000025)`. If any decider
returns "resume execution" for it, Windows re-raises `0xC0000025` (the OS cannot resume a
non-continuable exception), the vectored handler runs the decider again -> repeat loop.
POSIX has no such constraint for SIGABRT. (With no decider and an enclosing `__except` the
raise still works, which is why tests pass.)

### 9.3 W8 [code-level, Windows, Low] `stdc_raise` mutates the caller's `EXCEPTION_RECORD`

`thrd_signal_handle_windows.c.ipp:332-341`: when `info != NULL` and room remains, the
function appends the `0xdeadbeefdeadbeef` marker and the raw context into
`info->ExceptionInformation[]` and bumps `NumberParameters` — mutating the caller's record
in place. If the caller passes a kernel-supplied `EXCEPTION_RECORD` (re-raising a genuine
fault from inside a filter — exactly the "pass on signal handling to this library" use
case documented in the header), the record the kernel will later inspect is altered. The
marker write is also racy if two threads re-raise through the same record.

### 9.4 W10 [code-level, Low] `signal_decider_destroy` double-destroy is an unguarded use-after-free

`thrd_signal_handle_common.ipp.ipp:757` (`free(p)`) unconditionally frees the handle. A
second `signal_decider_destroy` on the same pointer reads freed memory before the
double-free — same class as the `siguninstall` double-free (M1). No guard exists.

### 9.5 X5 [code-level, POSIX, Low] `stdc_raise` returns `true` even when the previous handler ignored the signal

`thrd_signal_handle_posix.c.ipp:394-397` — when the map has an entry for the signal but no
global decider claims it, `stdc_raise` calls `invoke_sigaction(&sa, ...)` and
unconditionally returns `true`. If the pre-library handler was `SIG_IGN` (or the default
action is ignore — SIGCHLD/SIGURG/SIGWINCH), `invoke_sigaction` returns `false` but
`stdc_raise` still returns `true`, violating the documented contract. Callers using the
documented `if(!stdc_raise(...)) { fall back }` idiom will not detect the silently-ignored
case.

### 9.6 X6 [code-level, Low] `siguninstall_system()` is a non-functional stub

`thrd_signal_handle_common.ipp.ipp:555-563` — the function only validates `version == 0`
and returns 0; it installs/removes nothing. The header documents it as "Uninstall a
previously system installed signal guard", but no system installation exists anywhere in
the codebase. An API that reports success for an operation it never performs is a latent
trap for future callers (and for the eventual C standard library integration this library
targets).

### 9.7 X11 [code-level, Low] `sigfence` with more than 8 arguments produces a confusing hard error

`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:96-106`) returns the 9th
argument as the count; `sigfence(a,...,i)` expands `WG14_SIGNALS_SIGFENCE_IMPL_i` — an
undefined identifier — yielding a cryptic compile error rather than a diagnostic about the
8-argument limit. (The 0-arg form works; verified.)

### 9.8 Z5 [code-level, Windows, Low] `siguninstall` clobbers an application-installed `SetUnhandledExceptionFilter`

`thrd_signal_handle_windows.c.ipp:516-519`: the library captures the
unhandled-exception filter present at first `siginstall` and restores exactly that filter
on full uninstall. If the application installs its own filter *after* the library's
`siginstall`, the library's `siguninstall` overwrites the application's filter with the
stale pre-library one. The unhandled-exception filter is a single process-global slot, and
ownership transfer on uninstall is asymmetric with the app's expectations. The POSIX
sibling is AA4: `uninstall_sighandler_impl`
(`thrd_signal_handle_posix.c.ipp:414-419`) restores `item->old_handler` — any
`sigaction()`/`signal()` call the application makes *after* `siginstall` is overwritten at
`siguninstall`, reverting the slot to the pre-library handler. The header's warning ("NOT
threadsafe with respect to other code modifying the global signal handlers") is framed as
a concurrency caveat and does not cover the sequential case.

### 9.9 Z7 [code-level, Low] `sigfillset_synchronous` / `_asynchronous_nondebug` / `_asynchronous_debug` crash on a NULL `set`

All three implementations do `memcpy(set, ..., sizeof(*set))` (POSIX) or the equivalent
(Windows) with no NULL check — a NULL argument is a NULL deref, unlike every other
argument-taking API in the library.

### 9.10 Z10 [code-level, Windows, Low, extends X9] `asynchronous_nondebug_sigset` silently omits most documented signals and includes the two `siginstall`-skipped ones

`thrd_signal_handle_windows.c.ipp:92-108` builds the nondebug set from only
`{SIGINT, SIGKILL, SIGSTOP, SIGTERM}`, whereas the header documents it as containing at
least SIGALRM, SIGCHLD, SIGCONT, SIGHUP, SIGINT, SIGKILL, SIGSTOP, SIGTERM, SIGTSTP,
SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGPOLL, SIGPROF, SIGURG, SIGVTALRM. Additionally
`SIGKILL` and `SIGSTOP` (which `siginstall` deliberately skips) are in the set, so
`siginstall(sigfillset_asynchronous_nondebug())` claims two signals that are never
installed. The debug set is the sibling defect (X9): `asynchronous_debug_sigset`
(`:116-123`) returns the *empty* set while the header documents "at least these POSIX
signals are within this set: SIGQUIT, SIGTRAP, SIGXCPU, SIGXFSZ" — a Windows consumer gets
a guard set that never matches anything.

### 9.12 AA5 [code-level, both backends, Low] a *global* decider returning `sig_decision_invoke_recovery` has divergent, undocumented semantics

The enum documentation (`thrd_signal_handle.h:254-257`) says `sig_decision_invoke_recovery`
is "Thread local signal deciders only", yet global deciders share the same `sig_decide_t`
type and both backends accept it. **POSIX** (`thrd_signal_handle_posix.c.ipp:384-389`):
`if(res)` treats *any* non-zero decision as "claim and `return true`" — for
`invoke_recovery` the raise is claimed, no recovery is ever called, and for a genuine
fault the handler returns and the faulting instruction re-executes (an infinite re-fault
livelock),
**even when a guarding `sigguarded` frame exists**. **Windows**
(`thrd_signal_handle_windows.c.ipp:430-443`): the same value causes a
`longjmp(tss->front->buf, 1)` into the top frame when one exists (or
`EXCEPTION_CONTINUE_EXECUTION` / NULL-deref otherwise). So one enum value produces "claim,
no recovery, re-fault" on POSIX and "unwind to top frame" on Windows. Neither backend
documents or diagnoses this for global deciders.

### 9.13 AA6 [code-level, Windows, Low] user `EXCEPTION_RECORD` parameters masquerade as `rsi->addr` / `rsi->error_code`

`thrd_signal_handle_windows.c.ipp:207-216` reads `ExceptionInformation[1]` and
`ExceptionInformation[2]` as `addr` and `error_code` with no `NumberParameters` check. For
a user raise via `stdc_raise(signo, info, ctx)` the array holds the *caller's own
parameters*, so deciders see arbitrary user data in `addr`/`error_code` (deterministic for
non-NULL `info`, not garbage). Any decider keying on the NTSTATUS in `error_code` gets
different values for user raises than for genuine faults.

### 9.14 AA7 [code-level, C++ conformance, Low] `calloc` allocates C++ objects containing `std::atomic_uint` members without starting their lifetime

`tss_async_signal_safe.c.ipp:96-112` (`tss_async_signal_safe_create`) and `:230-241`
(`deinit_state` allocation) use `calloc` for structs whose members include
`std::atomic_uint` (the `lock` and `count` fields). In C++ — the library is documented and
tested as C++-usable, and `thread_atexit` is compiled as C++ — no constructor runs for
those atomics, so using them is object-lifetime UB per the C++ standard (works on
MSVC/GCC/Clang because `std::atomic<unsigned>` is trivially default-constructible in
practice). The C path is unaffected (C11 `atomic_uint` is a plain type).

### 9.15 AA9 [code-level, Windows, Low] `stdc_raise(SIGFPE)` raises a different exception code than a genuine integer divide-by-zero

`win32_exception_code_from_signal` maps SIGFPE -> `EXCEPTION_FLT_INVALID_OPERATION`
(0xC0000090), but the real hardware fault from `x / 0` on x64 is
`EXCEPTION_INT_DIVIDE_BY_ZERO` (0xC0000094); both reverse-map to SIGFPE. A decider that
inspects `rsi->error_code` (documented as "the NTSTATUS code") therefore observes different
codes for the same logical signal depending on whether it was user-raised or a real fault.
Similarly `stdc_raise(SIGBUS)` maps to `EXCEPTION_IN_PAGE_ERROR`, a semantically different
fault class from a real SIGBUS-equivalent. Cosmetic divergence, but the README/header
invite reading `error_code`.

---

## 10. Priority-ordered remediation summary

| ID | Severity | Issue | Location |
|----|----------|-------|----------|
| V2 | High | Windows vectored handler NULL-derefs `tss->front` on fresh threads | `thrd_signal_handle_windows.c.ipp:434-441` |
| V4 | High | Windows `stdc_raise` aborts for all unsupported signos | `thrd_signal_handle_windows.c.ipp:131-153` |
| 2.5 | Med | `thread_init` returns success for NULL item | `tss_async_signal_safe.c.ipp:209-217` |
| 2.6 | Med | `tss_async_signal_safe_*` NULL handle crash (also Z8, X8) | `tss_async_signal_safe.c.ipp:96-272` |
| 3.1 | Med | Spinlock not async-signal-safe | `lock_unlock.h` |
| 3.3 | Med | `SA_NOCLDWAIT`/`SA_NODEFER`/no `SA_RESTART` semantics | `thrd_signal_handle_posix.c.ipp:400-412` |
| 8.1 | Low | Post-longjmp access to modified non-volatile locals | `thrd_signal_handle_posix.c.ipp:264-287` |
| V5 | Med | Windows global deciders run twice per exception (once under a debugger) | `thrd_signal_handle_windows.c.ipp:486-519` |
| V6 | Med | setjmp-buffer race: frame published before setjmp completes | `thrd_signal_handle_posix.c.ipp:271-272`, `thrd_signal_handle_windows.c.ipp:315-316` |
| V7 | Med | `siguninstall` during another thread's `sigguarded` frees live TSS (fallback path) | `thrd_signal_handle_common.ipp.ipp:479-482` |
| W3 | Med | nested delivery overwrites the frame `rsi` mid-decider (SA_NODEFER re-entrancy) | `thrd_signal_handle_posix.c.ipp:312-315` |
| W6 | Med | `PROJECT_IS_TOP_LEVEL` needs CMake >= 3.21 vs `minimum_required(3.15)` — tests silently skipped | `CMakeLists.txt:1,231` |
| X4 | Med | Windows `EXCEPTION_STACK_OVERFLOW` unmapped (POSIX handles it as SIGSEGV) | `thrd_signal_handle_windows.c.ipp:154-183` |
| Y2 | Med | user `longjmp` out of `guarded()` -> dangling `tss->front` -> stack UAF in `stdc_raise` (confirmed) | `thrd_signal_handle_posix.c.ipp:249-287,310` |
| 8.6 | Med | never-returning global decider (e.g. `siglongjmp`) leaks node + container refcounts forever | `thrd_signal_handle_posix.c.ipp:358-369`, `thrd_signal_handle_windows.c.ipp:404-416` |
| Z2 | Low-Med | `stdc_raise(signo,NULL,NULL)` hands NULL `siginfo_t*`/`ucontext_t*` to pre-existing SA_SIGINFO handler (confirmed) | `thrd_signal_handle_posix.c.ipp:396` |
| X10 | Med-Low | musl builds fail to compile (`struct __siginfo` fallback) | `thrd_signal_handle.h:341` |
| W7 | Low | MSVC library build lacks `/WX` | `CMakeLists.txt:188-192` |
| 5.2 | Low | static lib requires an undeclared C++ runtime on non-`__cxa_thread_atexit()` platforms | `CMakeLists.txt:80-84`, `cmake/ProjectConfig.cmake.in` |
| Y7 | Low | `project()` unconditionally requires CXX; C-only toolchains cannot configure | `CMakeLists.txt:19` |
| W8 | Low | `stdc_raise` mutates caller's `EXCEPTION_RECORD` in place | `thrd_signal_handle_windows.c.ipp:332-341` |
| W9 | Low | longjmp skips C++ destructors in guarded frames (UB; 4611 disabled) | `thrd_signal_handle_posix.c.ipp:332`, `thrd_signal_handle_windows.c.ipp:440` |
| W10 | Low | `signal_decider_destroy` double-destroy = unguarded UAF | `thrd_signal_handle_common.ipp.ipp:757` |
| X5 | Low | `stdc_raise` returns true when the previous handler ignored the signal | `thrd_signal_handle_posix.c.ipp:394-397` |
| X6 | Low | `siguninstall_system` is a no-op stub that reports success | `thrd_signal_handle_common.ipp.ipp:555-563` |
| X7 | Low | out-of-range `signo` -> `sigismember` shift UB on macOS/BSD (glibc safe) | `thrd_signal_handle_posix.c.ipp:312` |
| X8 | Low | `tss_async_signal_safe_create` validates neither argument | `tss_async_signal_safe.c.ipp:96-112` |
| X9 | Low | Windows `asynchronous_debug_sigset` returns empty set, contradicts docs | `thrd_signal_handle_windows.c.ipp:116-128` |
| X11 | Low | `sigfence` with >8 args -> cryptic error | `thrd_signal_handle.h:96-106` |
| X12 | Low | `thrd_join` error check is wrong (`ret != -1`); `thrd_create` unchecked calloc | `test/test_common.h:81-100` |
| Y5 | Low | no `pthread_atfork`; stale TID caches/map across `fork()` | `current_thread_id.c.ipp:64-70,92-102`, `tss_async_signal_safe.c.ipp:84-94` |
| Y6 | Low | missing `NSIG` -> zero-length array + silently no-op `siginstall` | `thrd_signal_handle_common.ipp.ipp:61-62,503` |
| Y9 | Low | forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles silently (refines 4.2) | `config.h:51-77` |
| Z4 | Low | negative `signo` -> UB frame-walk shift / Windows `abort()` (extends X7/V4) | `thrd_signal_handle_posix.c.ipp:312`, `thrd_signal_handle_windows.c.ipp:131-153` |
| Z5 | Low | Windows `siguninstall` clobbers an app filter installed after `siginstall` | `thrd_signal_handle_windows.c.ipp:516-519` |
| Z6 | Low | `siginstall(NULL)` on glibc installs over `SIGCANCEL`/`SIGSETXID` (32/33) and realtime 34-64 | `thrd_signal_handle_posix.c.ipp:400-412,162-184` |
| Z7 | Low | `sigfillset_*` NULL `set` -> `memcpy` NULL-deref | both backends |
| Z8 | Low | `tss_async_signal_safe` NULL/double destroy + post-destroy `get`/`thread_init` unguarded | `tss_async_signal_safe.c.ipp:114-138,255-272` |
| Z9 | Low | `thread_init`'s unlocked `attr.create` breaks THREADSAFE claim for concurrent first-use | `tss_async_signal_safe.c.ipp:209-217` |
| Z10 | Low | Windows nondebug set omits documented signals, includes `SIGKILL`/`SIGSTOP` (extends X9) | `thrd_signal_handle_windows.c.ipp:92-108` |
| AC2 | Low | C `thread_atexit` swallows a failed `__cxa_thread_atexit()` registration (returns 0 unconditionally) | `thread_atexit.c.ipp:66-76` |
| AC3 | Low | fallback path: `sig_global_tss_state_create` overwrites and leaks an existing TSS | `thrd_signal_handle_common.ipp.ipp:342-359` |
| AC4 | Low | `sigfence` volatile-sink fallback rejects `volatile`/`const` args under `-Werror` (fixed) | `thrd_signal_handle.h:193` |
| AA4 | Low | `siguninstall` (POSIX) discards post-`siginstall` app handler changes (Z5 sibling) | `thrd_signal_handle_posix.c.ipp:414-419` |
| AA5 | Low | global decider `invoke_recovery`: POSIX claims-without-recovery vs Windows unwinds-to-frame | `thrd_signal_handle_posix.c.ipp:384-389`, `thrd_signal_handle_windows.c.ipp:430-443` |
| AA6 | Low | Windows user `EXCEPTION_RECORD` params leak into `rsi->addr`/`error_code` | `thrd_signal_handle_windows.c.ipp:207-216` |
| AA7 | Low | C++ object-lifetime UB: `calloc` for `std::atomic_uint` members | `tss_async_signal_safe.c.ipp:100-112,230-241` |
| AA8 | Low | `my_current_thread_id` cache uses plain `_Thread_local`, not async-safe TLS | `tss_async_signal_safe.c.ipp:84-94` |
| AA9 | Low | `stdc_raise(SIGFPE)` code != real `INT_DIVIDE_BY_ZERO` code; `SIGBUS`->`IN_PAGE_ERROR` | `thrd_signal_handle_windows.c.ipp:131-153` |

## 11. Methodology notes

- All library code (headers, `.ipp` files, sources, tests, CMake, `verstable.h`, the CI
  matrix, toolchain files, and the README) was read in full across the seven passes; all
  control-flow and error paths in `tss_async_signal_safe`, the common core, and both
  backends were traced, with fresh attention each pass to a different concern (lock
  pairing, half-committed error paths, frame-stack lifetime, sequential API sequences,
  build-config matrix, the `NSIG >= 1024` branch, hand-off behaviour, post-uninstall use).
- Live verification was performed on macOS arm64 (clang 17, CMake 4.3.2) against the
  ASan/UBSan sanitized static library **rebuilt from the current tree** — a pass-4
  methodology trap: header-level patches linked against the prebuilt library are silently
  ignored (the implementation comes from the library's `.o` files), so the library itself
  must be rebuilt from any patched tree.
- Windows findings are code-level (no Windows host available) and were verified against
  the MSVC CI build matrix.
- One-off repro programs from the review scratch dir were temporary artifacts and are not
  part of the tree.
