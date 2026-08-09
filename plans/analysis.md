# Exhaustive implementation analysis: wg14_signals

Review date: 2026-08-05. Revision reviewed: `f48e95e` ("Implement all the changes as per
N3924 WIP wording for 'Thread-safe signals handling rev 4'"), plus one uncommitted
whitespace/`nullptr`-for-C++ change in `config.h`.

**A sixth full review pass was performed on 2026-08-06 (same revision + the same
uncommitted `config.h` delta). Section 16 records its live verification results, four
corrections/refinements to earlier passes, and eleven new findings — including a verified
heap-use-after-free on the destroyed TSS handle after a full `siguninstall` on the fallback
path (Z3, a purely sequential form of the 2.4/V7 dead-reset bug), a verified NULL-`siginfo`
hand-off to pre-existing `SA_SIGINFO` handlers from `stdc_raise` (Z2), and the never-
initialised verstable `signo_to_sighandler_map_t` on NSIG>=1024 platforms (Z1).**

**A seventh full review pass was performed on 2026-08-06 (same revision + the same
uncommitted `config.h` delta). Section 17 records its live verification results and nine
new findings — including a verified, purely sequential NULL-deref/heap crash in
`signal_decider_destroy` after a `siguninstall` → `siginstall` cycle orphans the decider
node (AA1), a verified test-suite build break under the undocumented
`WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` (AA3), and confirmation that the filc toolchain's
`-DDISABLE_INLINE_ASM=1` flag is a no-op for the library's own inline-asm path (AA2).**

**A fourth full review pass was performed on 2026-08-06 (same revision + the same
uncommitted `config.h` delta). Section 14 records its live verification results, two
corrections to earlier passes (including one refuted first-pass claim), and eleven new
findings — including two verified sequential (non-concurrent) heap use-after-frees in
`tss_async_signal_safe` (the `deinit_state` lifetime bug family, X1/X2).**

**A fifth full review pass was performed on 2026-08-06 (same revision + the same
uncommitted `config.h` delta). Section 15 records its live verification results, two
corrections/refinements to earlier passes (including a refutation of the claimed
Windows global-decider-before-frame ordering), and ten new findings — including a
second verified spinlock leak in the same bug family as 1.1 (Y1, High, the
`install_sighandler` failure path), a verified stack-use-after-scope from a user
`longjmp` escaping `sigguarded` (Y2), and a verified duplicate-symbol failure mode for
multi-TU C header-only consumers (Y10).**

Scope: every header, every source file, both backends (POSIX/Windows), the header-only
configuration, the fallback hash-table TLS path and the async-signal-safe TLS path, all
error paths, and all build configurations exercised and *not* exercised by CI.

Findings are ranked by severity. Items marked **[confirmed]** were reproduced on macOS
(arm64, clang 17, ASan where noted). Windows-only items are code-level findings (no
Windows host available) but were verified against the MSVC build matrix in CI.

**A second full review pass was performed on 2026-08-05 (same revision). Section 12
records the live verification results for the first-pass findings, corrections to four
first-pass claims, and new findings (including two verified packaging defects: the
installed package ships no headers and `find_package()` hard-fails).**

---

## 1. Critical bugs

### 1.1 `signal_decider_create()` leaks the global state spinlock permanently [FIXED]

`include/wg14_signals/detail/impl/thrd_signal_handle_common.ipp.ipp:500-514`:

```c
LOCK(state->lock);
it = signo_to_sighandler_map_t_get(&state->signo_to_sighandler_map, signo);
if(signo_to_sighandler_map_t_is_end(it))
{
  WG14_SIGNALS_STDERR_PRINTF(
    "WARNING: signal_decider_create() installing decider for signal %d but "
    "handler was never installed for that signal.\n", signo);
  continue;                 /* <-- `continue` skips the `UNLOCK` at line 540 */
}
```

If any signal in the guarded set has no installed handler, the warning is printed and
the loop `continue`s **without calling `UNLOCK(state->lock)`**. The global spinlock is
then held forever:

- Every subsequent call to `stdc_raise`, `siginstall`, `siguninstall`,
  `signal_decider_create`, or `signal_decider_destroy` spins forever (they all
  `LOCK(state->lock)`).
- It can even deadlock a live signal handler, because `raw_signal_handler` ->
  `stdc_raise` also takes this lock.

Reproduction: install a handler for `SIGUSR2` only, then
`signal_decider_create({SIGUSR1, SIGUSR2})`. The WARNING prints and the process hangs
in the next library call (verified: 100% CPU spin inside `signal_decider_create`'s
`LOCK` with the lock value observed to be left =1).

Fix: `UNLOCK(state->lock)` before `continue`.

**Status: FIXED (2026-08-09).** `UNLOCK(state->lock)` was added at
`thrd_signal_handle_common.ipp.ipp:513` before the warning-path `continue`. Verified by a
rebuild plus `ctest -E benchmark` (4/4 pass). A regression test
(`test/decider_mixed_set_test.c`, TIMEOUT 60) now covers the mixed-set scenario: without
this unlock the test hangs forever inside `signal_decider_destroy`.

### 1.2 `signal_decider_create()` / `signal_decider_destroy()` slot misalignment -> use-after-free [FIXED]

Even with the lock leak of 1.1 fixed, the decider handle layout is corrupted whenever a
guarded signal has no installed handler:

- `signal_decider_create` advances the slot pointer `*retp++` **only** for guarded
  signals that have an installed handler (the warning path does not advance it).
- `signal_decider_destroy` advances `retp++` for **every** guarded signal
  (`thrd_signal_handle_common.ipp.ipp:603-610`), regardless of whether the signal has a
  handler.

When a non-installed signal is numerically *smaller* than an installed one in the same
guarded set, the slot pointers misalign: `destroy` reads slot[0] (the node registered
for the installed signal) while iterating the *non-installed* signal, and — because the
map lookup for that signal returns end — it skips the in-lock bookkeeping
(`LIST_REMOVE`, refcount) and then executes the out-of-lock `if(*retp != NULL)
free(*retp);`. The node is freed while still linked into the *other* signal's
`global_handler` list. The next raise of that signal dereferences freed memory.

Reproduction (with 1.1 patched): install `SIGUSR2`; create a decider for
`{SIGUSR1, SIGUSR2}`; destroy it; then `stdc_raise(SIGUSR2)` -> SIGBUS
(EXC_BAD_ACCESS) inside `stdc_raise` walking the freed node list.

The two bugs together mean `signal_decider_create` with any mixed installed/not-installed
set is unusable: either the library deadlocks (1.1) or, after fixing 1.1, the handle is
corrupt and destruction crashes the next raise (1.2). The warning message
explicitly anticipates this input, so it is not an exotic misuse.

Fix: make slot advancement identical in both functions (or store the list of installed
signal numbers in the handle instead of a positional array).

**Status: FIXED (2026-08-09).** The warning path of `signal_decider_create` now advances
the handle slot with `*retp++ = WG14_SIGNALS_NULLPTR` at
`thrd_signal_handle_common.ipp.ipp:514`, so `create` and `destroy` both advance exactly
one slot per guarded signal and NULL slots are skipped. Verified: the reproduction
(install SIGUSR2 only; create + destroy a decider for {SIGUSR1, SIGUSR2}; raise SIGUSR2)
crashes with an ASan heap-use-after-free in `stdc_raise` against the pre-fix library and
runs 100 iterations cleanly against the fixed build; `ctest -E benchmark` passes 4/4. A
regression test (`test/decider_mixed_set_test.c`, registered in `test/CMakeLists.txt` with
TIMEOUT 60) runs the mixed-set create/destroy/raise cycle 10 times; with the alignment
reverted it fails its `CHECK(signal_decider_destroy == 0)` and ASan reports the same
heap-use-after-free in `stdc_raise`.

### 1.3 `sigguarded()` / `stdc_raise()` NULL-pointer dereference on fallback-TLS platforms when `siginstall()` was never called [FIXED]

On platforms with `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL == 0` (Apple/macOS, and any
platform not GNU/MSVC), the thread-local state object is created only inside
`install_sighandler()` (`thrd_signal_handle_common.ipp.ipp:318`). `sig_global_tss_state_init()`
(`:264-268`) unconditionally calls:

```c
return tss_async_signal_safe_thread_init(*sig_tss_state_raw());
```

`*sig_tss_state_raw()` is a NULL static until `siginstall()` has been called at least
once. `tss_async_signal_safe_thread_init(NULL)` dereferences `mem->lock` immediately
(`tss_async_signal_safe.c.ipp:177`) -> SIGSEGV.

This means on macOS (the reference platform for the fallback path) the documented
"standalone" uses crash:

- `sigguarded(...)` without a prior `siginstall()` -> crash (reproduced: EXC_BAD_ACCESS
  at `tss_async_signal_safe_thread_init(val=0x0)`, address 0x10).
- The header's recommended one-line setup `stdc_raise(0, nullptr, nullptr)`
  (`thrd_signal_handle.h:344-346`) -> same crash on the fallback path, because it
  returns only *after* `sig_global_tss_state_init()`.

The header documents sigguarded/stdc_raise as usable without any other setup step; the
dependency on `siginstall()` is real and unstated. Linux is unaffected because there
`sig_global_tss_state_create` is a no-op and the TLS pointer is used directly.

**Status: FIXED (2026-08-09).** The fallback `sig_global_tss_state_init`
(`thrd_signal_handle_common.ipp.ipp:264-272`) is now self-creating: it calls
`sig_global_tss_state_create()` when `*sig_tss_state_raw()` is NULL before calling
`tss_async_signal_safe_thread_init()`, mirroring the native-TLS path. Verified: the new
regression test `test/standalone_setup_test.c` (registered in `test/CMakeLists.txt`) calls
`stdc_raise(0, NULL, NULL)` and a bare `sigguarded(...)` with no prior `siginstall()`; it
crashed with the ASan SEGV at `tss_async_signal_safe.c.ipp:177` (address 0x10) before the
fix and passes 6/6 under the sanitizer build afterwards. (Windows does not exercise the
`stdc_raise(0, ...)` leg here because of the unfixed 1.4 `abort()`.)

### 1.4 Windows: `stdc_raise(0, ...)` aborts the process

`thrd_signal_handle_windows.c.ipp:252-300`: the Windows `stdc_raise` has **no
`signo == 0` short-circuit** (the POSIX backend has one at
`thrd_signal_handle_posix.c.ipp:281-285`). It falls straight into:

```c
const DWORD win32sehcode = win32_exception_code_from_signal(signo);
```

and `win32_exception_code_from_signal` (`:112-134`) executes `default: abort();` for any
unrecognised code. Therefore the documented setup call `stdc_raise(0, nullptr, nullptr)`
calls `abort()` on Windows. This is a direct, platform-visible crash of a documented API.
**Note (see 12.3, V4): the same `abort()` fires for *every* signo outside
{SIGABRT, SIGBUS, SIGILL, SIGSEGV, SIGFPE} — e.g. `stdc_raise(SIGINT)`,
`stdc_raise(SIGTERM)`, `stdc_raise(SIGPIPE)` — so the defect is broader than `signo == 0`.**
On the fallback-TLS platforms the same documented setup call segfaults instead (1.3), so
the recommended one-line setup crashes on every non-Linux platform.

### 1.5 Windows: `prepare_rsi()` reads `ExceptionInformation[]` out of bounds

`thrd_signal_handle_windows.c.ipp:166-192`:

```c
rsi->error_code = ... ptrs->ExceptionRecord->ExceptionInformation[2];  // NTSTATUS
rsi->addr       = ... ptrs->ExceptionRecord->ExceptionInformation[1];
```

There is no check on `NumberParameters`. The `ExceptionInformation` array has
`EXCEPTION_MAXIMUM_PARAMETERS` (15) slots but its valid prefix length is
`NumberParameters`. `stdc_raise` with `info == NULL` calls
`RaiseException(win32sehcode, 0, 0, NULL)` — zero parameters — so both reads are
out-of-bounds (past the end of the parameters array of the EXCEPTION_RECORD). The
bounds check that does exist (`:172-175`) only guards the `0xdeadbeef...` marker
detection for the `raw_context` slot. Any SEH-raised exception with <3 parameters that
reaches `win32_exception_filter` or `win32_vectored_exception_function` triggers the
OOB read.
**Correction (see 12.2, C4): the reads are within the fixed 15-slot array, i.e. not a
heap overrun, but they read past `NumberParameters`. A genuine x64 access violation has
`NumberParameters == 2`, so `ExceptionInformation[2]` (the "NTSTATUS") is *uninitialised
garbage for virtually every real fault*, not just for the `info == NULL` raise path.**

### 1.6 Windows: thread-local frame stack (`tss->front`) is left dangling / never maintained

The Windows `sigguarded` uses `__try/__except` directly and **never pushes a frame onto
`tss->front`** (unlike POSIX). The Windows `stdc_raise` pushes a frame
(`thrd_signal_handle_windows.c.ipp:265-271`) but only pops it in the `setjmp`-return
path:

```c
tss->front = &current;                       /* current is a stack local */
if(setjmp(current.buf) != 0) { tss->front = old; return true; }
...
RaiseException(...);                          /* if this returns (no handler
                                                 longjmped) tss->front is NEVER reset */
return true;                                  /* <-- frame left pointing at dead stack */
```

Consequences:

- After any `stdc_raise` completes (exception caught by an enclosing `__except`,
  `RaiseException` returning, or the longjmp path unwinding elsewhere), `tss->front`
  points at a dead stack frame.
- `win32_vectored_exception_function` (`:357-363`) then does
  `if(tss->front != NULL) longjmp(tss->front->buf, 1);` when a global decider returns
  "resume" — longjmp into a dead frame -> UB/crash for any real exception (access
  violation etc.) that is not caught by a frame `__except`.
- Conversely, when a *real* fault occurs inside a `sigguarded` region (no
  `stdc_raise` in the call stack), `tss->front` is NULL, so a global decider returning
  "resume" falls through to `return EXCEPTION_CONTINUE_EXECUTION` (`:366`) — the
  faulting instruction re-executes forever (infinite fault loop), because
  `EXCEPTION_CONTINUE_EXECUTION` on a non-recoverable fault just re-faults.
- POSIX and Windows also differ in decider ordering: POSIX consults thread-local frames
  first, then global deciders; Windows runs vectored (global) deciders *first*, then the
  frame `__except` filter. This is an undocumented semantic inconsistency between
  backends.

The Windows tests only ever exercise `stdc_raise` inside a `sigguarded` that handles the
exception via `__except`, so these paths are untested.
**Additionally (see 12.3, V2): when a global decider claims an exception on a thread on
which `sigguarded`/`stdc_raise` was *never* called, `sig_global_tss_state()` returns a
NULL per-thread state (the vectored handler never runs `sig_global_tss_state_init`, and
the TLS pointer is created lazily per thread) and `tss->front` dereferences NULL —
a crash *inside* the exception handler. This is a distinct failure from the dangling
frame above, and it is reachable by a genuine fault on any fresh thread.**

### 1.7 Windows: NULL `recovery` + `sig_decision_invoke_recovery` -> infinite fault loop

`thrd_signal_handle_windows.c.ipp:210-213`:

```c
case WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery):
  return (recovery != WG14_SIGNALS_NULLPTR) ? EXCEPTION_EXECUTE_HANDLER
                                            : EXCEPTION_CONTINUE_EXECUTION;
```

If the decider chooses "invoke recovery" but the user supplied a NULL recovery function,
Windows resumes execution at the faulting instruction (`EXCEPTION_CONTINUE_EXECUTION`).
For a real fault (divide-by-zero, access violation) this re-faults indefinitely — a
hang. **Correction (see 12.2, C1): the POSIX backend does *not* "fall through to the
next decider / previous handler" for this case as originally claimed — it executes
`return true` (`thrd_signal_handle_posix.c.ipp:302-307`), which for a real fault makes
the handler return and the faulting instruction re-execute — the identical infinite
fault loop. The backends livelock alike; only the mechanism differs.**

### 1.8 Header-only build is entirely broken [confirmed]

- `-DHEADER_ONLY_BUILD=ON` fails to compile the library itself. Confirmed errors:
  - `redefinition of 'get_current_thread_id'` and `redefinition of
    'internal_current_thread_id_cached_set'` in `current_thread_id.c.ipp` (the .ipp is
    pulled in twice in one TU: once by `current_thread_id.c` including the .ipp, which
    includes `current_thread_id.h`, which in header-only mode includes the .ipp again).
  - `redefinition of 'thread_atexit'` in `thread_atexit.cpp` — `thread_atexit.cpp.ipp`
    has no include guard and includes `thread_atexit.h`, which in header-only C++ mode
    includes `thread_atexit.cpp.ipp` recursively.
  - `-Werror=static-in-inline`: headers declare functions `WG14_SIGNALS_EXTERN` =
    `inline` (external linkage) while the .ipp definitions call `static` helpers
    (`my_current_thread_id`, verstable's static-inline functions, etc.).
  - `-Werror=static-local-in-inline` in `sig_global_state()`.
- The pure source-level header-only path (the `header_only_test` target, which *does not
  link the library*) happens to work in C++ because the header's `inline` declarations
  give the out-of-line definitions inline (COMDAT) linkage, letting the linker merge
  duplicates across TUs. But:
  - **Correction (see 12.2, C3): C-language header-only consumers fail with
    *undefined symbols*, not duplicate-symbol link errors.** Per C11 6.7.4p7, when all
    file-scope declarations carry `inline`, the .ipp definitions are *inline
    definitions* that provide no external symbol; cross-TU calls and any call the
    compiler fails to inline (e.g. `-O0`) produce undefined references, and
    `thread_atexit()` is not provided at all in C (`thread_atexit.h:41`), so
    `tss_async_signal_safe` and the signal-handle code do not link. Both failure modes
    were reproduced (see 12.1).
  - In C header-only, `thread_atexit()` is deliberately not provided
    (`thread_atexit.h:41`), so `tss_async_signal_safe` and the signal-handle code do not
    link at all.
  - The library's own TU compiles under `-Werror` fail as above, so the CMake option is
    unusable, and no CI configuration ever builds it.

---

## 2. High-severity issues (static analysis)

### 2.1 `tss_async_signal_safe` deinit race -> use-after-free of the `deinit_state`

`tss_async_signal_safe.c.ipp:136-168`. The thread-exit callback
`tss_async_signal_safe_thread_deinit` decrements `state->count` and frees `state`
*outside* `mem->lock` (after `UNLOCK` at line 160). Two threads sharing the same `tss`
that exit at roughly the same time can interleave:

1. Thread A: destroy/erase its own entry, `UNLOCK`, then `fetch_sub(&count)`.
2. Thread B: destroy/erase its own entry, `UNLOCK`, `fetch_sub(&count)` -> returns 1 ->
   `free(state)`.
3. Thread A then executes `atomic_fetch_sub(&state->count, ...)` on freed memory (UAF),
   or reads `state->val` concurrently with `tss_async_signal_safe_destroy`'s
   `state->val = NULL` write (data race / UAF — destroy reads/writes `state->val` under
   `mem->lock`, deinit reads it without the lock at line 140).

Additionally `tss_async_signal_safe_destroy` itself frees `mem` while other threads'
atexit handlers may still hold `state->val == mem`; if a deinit runs concurrently it can
lock a freed object. The intended usage (destroy only after all threads joined) is not
documented.

### 2.2 `siguninstall` / `signal_decider_destroy` vs. in-flight `stdc_raise` -> use-after-free of `sighandler_info`

In `stdc_raise` (POSIX, `thrd_signal_handle_posix.c.ipp:314-368`) the `state->lock` is
released around each `current->decider(&rsi)` call. If another thread runs
`siguninstall` during that window and the per-signal refcount drops to zero, it frees
the `sighandler_info` container (the map entry holding `global_handler`/`deferred_frees`
list heads) while `stdc_raise` still holds the `it` pointer and re-accesses
`value(it)->global_handler` after re-acquiring the lock -> UAF. The existing
refcount scheme protects the `global_signal_decider_t` nodes (exercised by the
"concurrent destroy" test) but not the container. The header documents siginstall as
threadsafe only with respect to other concurrent executions of itself
(`thrd_signal_handle.h:414-416`), so concurrent `siguninstall` + `stdc_raise` is not
formally promised — but nothing prevents the user from doing it, and the API returns
errors/values that imply safety.

### 2.3 `install_sighandler` increments `sighandlers_count` before checking TSS creation

`thrd_signal_handle_common.ipp.ipp:315-323`:

```c
if(0 == state->sighandlers_count++) {
  if(-1 == sig_global_tss_state_create()) { UNLOCK; return false; }
}
```

If `sig_global_tss_state_create()` fails (fallback path: `calloc` failure), the function
returns false but `sighandlers_count` was already incremented and the map entry was
already installed. The caller (`siginstall`) treats failure as fatal and returns NULL,
leaving a handler installed that can never be uninstalled (no handle), and the count
desynchronised from reality. A subsequent `siguninstall` on a later handle would call
`sig_global_tss_state_destroy()` on a TSS that was never created (NULL) — which in the
fallback path dereferences NULL inside `tss_async_signal_safe_destroy` (see also 2.6).

### 2.4 `sig_global_tss_state_destroy` contains dead code

`thrd_signal_handle_common.ipp.ipp:276-281`:

```c
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_destroy)(void)
{
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
      *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)() = WG14_SIGNALS_NULLPTR;  /* never runs */
}
```

The intended reset of the static TSS slot to NULL is unreachable. After a full
uninstall, `*sig_tss_state_raw()` dangles (freed TSS). Any later
`stdc_raise`/`sigguarded` on the fallback path before a re-`siginstall` uses the freed
TSS (UAF). It is masked only because `sig_global_tss_state_create()` always overwrites
the slot on re-install.

### 2.5 `tss_async_signal_safe_thread_init` returns success when `create` yields NULL

`tss_async_signal_safe.c.ipp:186-190`:

```c
int ret = mem->attr.create(&newitem);
if(ret != 0 || newitem == WG14_SIGNALS_NULLPTR)
{
  return ret;   /* returns 0 (success) if create()==0 but newitem==NULL */
}
```

A `create` callback that returns 0 but leaves `*dest` NULL makes `thread_init` report
success; the subsequent `insert(..., newitem=NULL)` stores NULL and the next
`tss_async_signal_safe_get` returns NULL.

### 2.6 `tss_async_signal_safe_destroy(NULL)` dereferences NULL

The fallback `sig_global_tss_state_destroy` (2.4) and user calls with a NULL/zeroed
handle crash: `tss_async_signal_safe_destroy(NULL)` ->
`LOCK(mem->lock)` on NULL. There is no validation of the handle in any of
create/destroy/thread_init/get.

### 2.7 `sigguarded`/`sigfpe` NULL-argument handling aborts the process

Both backends `abort()` if `signals`, `guarded`, or `decider` is NULL
(`thrd_signal_handle_posix.c.ipp:229-233`, Windows `:227-231`). A library aborting on
argument errors is a harsh but deliberate design decision; it is inconsistent with the
rest of the API which returns error codes, and it makes the failure mode a process crash
rather than an error return. (The `sig_decision_invoke_recovery` decider returning with
a NULL `recovery` is legal per the docs, but the abort-on-NULL applies only to the three
top-level arguments.)

### 2.8 `sigfence` on GNU compilers requires lvalues; non-lvalues fail to compile

`SIGFENCE_IMPL_1(a)` expands to `__asm__ volatile(";" : "+m"(a) : : "memory")`. The
`+m` operand must be an lvalue: `sigfence(42)` or `sigfence(x + 1)` is a hard compile
error. The doc says the argument list is "for local variables", which is accurate, but
nothing prevents (and nothing diagnoses) rvalue usage.

### 2.9 `sigfence` fallback ("escaped") relies on the function being externally visible; broken under LTO

`sigfence_force_escaped` (`sigfence_force_escaped.c.ipp:30-38`) never reads its variadic
arguments; the "escape" effect relies on the compiler not being able to see the
function body. Under `-flto` the body is visible and the spill/reload can be elided,
silently weakening the fence.

---

## 3. Medium-severity issues

### 3.1 Spinlock is not async-signal-safe (deadlock risk in signal handlers)

`LOCK`/`UNLOCK` (`lock_unlock.h:33-61`) is a CAS spinlock with no signal masking, no
backoff, and no `pause`/`yield`. It is used inside signal-handler contexts on both
backends (`stdc_raise` -> `LOCK(state->lock)`, Windows vectored handler ->
`LOCK(state->lock)`, and the fallback `tss_async_signal_safe_get` ->
`LOCK(mem->lock)`). If a signal is delivered while the interrupted thread is itself
holding the same lock (e.g., the main thread inside `siginstall`/`signal_decider_create`
for `state->lock`, or inside `tss_async_signal_safe_get` for `mem->lock`), the handler
spins forever — a silent deadlock. The header's "usually async signal safe" claim
(`thrd_signal_handle.h:328-347`) does not cover re-entrancy. This is a fundamental
limitation of the lock design for the advertised property.

### 3.2 First signal delivery on a fresh thread performs allocation inside the handler

`stdc_raise` -> `sig_global_tss_state_init` -> `calloc` + `thread_atexit` (which does
`std::vector` allocation / possibly throws) on the first call per thread
(`thrd_signal_handle_common.ipp.ipp:206-227`, `thread_atexit.cpp.ipp:57-64`). If the
first signal ever delivered to a thread arrives before any library call on that thread,
malloc and C++ heap operations run inside the handler (not async-signal-safe; risk of
deadlock on the heap lock). The docs recommend pre-calling `stdc_raise(0, ...)` — which,
per 1.3/1.4, crashes on both Windows and fallback-TLS platforms. On Linux this works,
but the safety relies entirely on the user reading the docs.

### 3.3 `SA_NOCLDWAIT` + `SA_NODEFER` + missing `SA_RESTART` alter process semantics

`install_sighandler_impl` (`thrd_signal_handle_posix.c.ipp:371-383`) installs with
`sa_flags = SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER` for **every** signal:

- `SA_NOCLDWAIT` on `SIGCHLD` changes the process's child-reaping semantics: children
  are auto-reaped and `waitpid`/`wait` return `ECHILD`. `siginstall(NULL)` (install all
  signals — exactly what the tests do) silently breaks the host application's `waitpid`
  behaviour for the whole tenure of the install. This flag is only meaningful for
  SIGCHLD and should not be applied to other signals, and applying it to SIGCHLD at all
  is a behavioural change that should be documented or avoided.
- `SA_NODEFER` permits re-entrant signal delivery; a fault in the handler re-enters it
  (infinite recursion / loops) — acknowledged in the docs, but dangerous.
- No `SA_RESTART`: syscalls interrupted by the installed signals return `EINTR` during
  the library's tenure, even if the original handler had `SA_RESTART` (the old flags are
  discarded). Uninstall restores the original handler but the application has already
  observed the changed EINTR behaviour.

### 3.4 `invoke_sigaction` default handling is wrong for stop/continue signals and re-raises under `SA_NODEFER`

`thrd_signal_handle_posix.c.ipp:143-184`: the "default is to ignore" list only covers
SIGCHLD/SIGURG/SIGWINCH. Signals whose default action is "stop" (SIGSTOP, SIGTSTP,
SIGTTIN, SIGTTOU, SIGCONT) fall into the "reset to SIG_DFL and `pthread_kill(self)`"
branch. Re-raising a stop signal via `pthread_kill` from within a handler whose
installation used `SA_NODEFER` will deliver the stop — that is the correct default, but
for a *signal handler* executing on another thread, the re-raise targets the current
thread. Also, re-raising with the handler reset to `SIG_DFL` permanently discards the
library's handler for that signal on that thread's process — subsequent raises are no
longer filtered (the map still claims the signal is installed, but the kernel handler
is now default). This can leave the library in a state where `siguninstall` restores a
handler that is no longer installed (harmless) but where "install reference counts"
believe the signal is still handled.

### 3.5 `raw_signal_handler` on unknown signals silently installs SIG_DFL and re-raises

`thrd_signal_handle_posix.c.ipp:203-219`: if `stdc_raise` returns false, the handler
replaces itself with `SIG_DFL` and invokes `invoke_sigaction(&sa, ...)` where `sa` is
the freshly-minted SIG_DFL struct — i.e., for a default-ignore signal it returns false
(no re-raise, signal silently dropped); for others it re-raises as default. This is
reasonable, but the comment admits "It shouldn't happen that this handler gets called
when we have no knowledge of the signal" — a defensive path with subtle behaviour.

### 3.6 Thread-ID reuse with stale hash-table entries

`tss_async_signal_safe` maps are keyed by kernel thread ID (`current_thread_id`). If a
thread exits without running its atexit deinit (abnormal termination, `_exit` within a
thread is process-wide, cancellation corner cases, or `thread_atexit` registration
failing), the map retains the entry under that TID. A later thread that reuses the same
TID will observe the *previous* thread's value (never its own), and destruction may run
with stale state. There is no TID-generation counter.

### 3.7 `sig_global_state_tss_state_init` failure inside `stdc_raise` hides the real error

`stdc_raise` returns `false` both for "no handler installed for this signal" and for
"TSS init failed" (`thrd_signal_handle_posix.c.ipp:277-285`). The POSIX `signo == 0`
setup call also returns false on init failure, so the documented setup call gives no
diagnostic when setup actually failed.

### 3.8 Partial install failure in `siginstall` leaves handlers installed (no rollback)

`thrd_signal_handle_common.ipp.ipp:381-405`: if `install_sighandler` fails for any
signal in the set, `siginstall` frees the returned handle and returns NULL, but the
signals already installed remain installed and counted. The caller has no handle and no
way to uninstall them; a subsequent `siginstall` will double-count and can reach the
deferred-free/TSS state inconsistently. `siguninstall` semantics ("threadsafe with
respect to other concurrent executions of itself") also allow concurrent partial
uninstall while handlers are in flight.

### 3.9 `signal_decider_create` failure path partially self-destroys correctly but leaves warning-path signals uncounted

When `calloc` fails mid-loop, `signal_decider_create` calls `signal_decider_destroy(ret)`
on the partially-built handle. With the 1.1/1.2 bugs fixed this is mostly coherent, but
note that `signal_decider_destroy` returns -1 (errno unchanged) when it finds no
matching slots — a misleading error signal for the caller. Additionally, `fprintf`
(`WG14_SIGNALS_STDERR_PRINTF`) runs while `state->lock` is held (line 508), which is
slow and can itself trigger a signal while the lock is held (see 3.1).

### 3.10 `signal_decider_destroy` frees nodes outside the lock

`thrd_signal_handle_common.ipp.ipp:603-608`: after decrementing a node's refcount to
zero under the lock and removing it from the list, `free(*retp)` runs after `UNLOCK`.
A concurrent `stdc_raise` that had already captured `current` but not yet incremented its
refcount could in principle race; in practice the refcount increment happens before the
unlocked decider call, so the current design is safe, but the free outside the lock is
fragile and undocumented.

### 3.11 `thread_atexit` C++ exceptions disabled -> OOM terminates

`thread_atexit.cpp.ipp:57-71`: with `-fno-exceptions` the `try/catch` block is compiled
out; `std::vector::emplace_back` on allocation failure calls `std::terminate` instead of
returning -1. The library is designed to be embedded in C standard libraries where
exceptions may be disabled; this path then crashes instead of reporting failure.

### 3.12 Function-pointer type pun for atexit callback

`tss_async_signal_safe.c.ipp:217-218` casts
`int (*)(struct deinit_state *)` to `void (*)(void *)` and registers it via
`thread_atexit`. Calling through an incompatible function-pointer type is UB per the C
standard (works on common ABIs, but is a latent portability hazard).

### 3.13 `tss_async_signal_safe_thread_init` re-entrancy (signal during `attr.create`) leaks

`tss_async_signal_safe.c.ipp:184-191` unlocks before calling the user's `create` and
re-locks before `insert`. If a signal handler runs `thread_init` on the same object in
that window (possible via `stdc_raise` -> `sig_global_tss_state_init` ->
`thread_init`), the user's `create` callback runs twice and the second `insert`
replaces the first entry: the first value leaks (no destructor on replace), the `count`
is double-incremented, and two atexit registrations are queued.

### 3.14 `tss_async_signal_safe_thread_init` does not roll back on `thread_atexit` failure

`tss_async_signal_safe.c.ipp:214-223`: the map entry and `state->count` are committed
before `thread_atexit` is called; if it returns -1 the caller sees failure but the entry
and count remain, and no thread-exit cleanup will ever run for this thread.

---

## 4. Portability and configuration concerns

### 4.1 Default `get_current_thread_id` fallback is FreeBSD-only

`current_thread_id.c.ipp:70-72`: the final `#else` branch calls
`pthread_getthreadid_np()`, which exists only on FreeBSD (the FreeBSD block also pulls
in `<pthread_np.h>` at line 36-38). On any other POSIX platform (Solaris, NetBSD,
OpenBSD, AIX, ...) the code fails to compile. The portable fallback should be
`(thread_id_t)pthread_self()`.

### 4.2 `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` auto-detection is too optimistic

`config.h:41-51` enables async-safe TLS for *any* `__GNUC__` (which includes clang) on
any non-Apple platform. This is only true where the toolchain actually supports
`tls_model("initial-exec")` and the libc reserves static TLS for dlopened libraries —
which the README itself documents as *not* universally true. On platforms where GCC
lacks the attribute (some embedded targets, older toolchains) or where runtime dlopen
fails, builds break or crash. Also, a user-defined
`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on a compiler that is neither `__GNUC__`
nor `_MSC_VER` leaves `WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL` undefined while code
references it (current_thread_id.h:50, current_thread_id.c.ipp:50,
thrd_signal_handle_common.ipp.ipp:201) -> compile error with no diagnostic.

### 4.3 `pthread_getthreadid_np` / `mach_thread_self` are not async-signal-safe

`current_thread_id.c.ipp:66-69`: the Apple branch performs `mach_port_deallocate`
(kernel round-trip) on every cache miss; `current_thread_id()` is documented as "ASYNC
SIGNAL SAFE" (current_thread_id.h:57). On fallback platforms the cache miss happens
inside a signal handler on first use -> async-signal-unsafe syscalls (and repeated on
every call when no caching layer exists — see `internal_current_thread_id_cached_set`).

### 4.4 MSVC CRT signals bypass SEH (Windows)

`siginstall` on Windows installs only SEH vectored handlers
(`thrd_signal_handle_windows.c.ipp:408-427`); it does not install CRT signal handlers.
`abort()`, `raise(SIGFPE)`, `assert`, etc. on MSVC dispatch through the CRT, which does
not raise SEH exceptions — so they never reach the library. Only genuinely
SEH-raised exceptions (access violations, integer overflow traps on x86, explicit
`RaiseException`) are handled. This is a substantial functional gap on the Windows
platform versus POSIX (where `abort()`/`raise()` *do* route through `sigaction`), and it
is not documented.

### 4.5 32-bit sigset_t on Windows overflows for signals >32

`thrd_signal_handle.h:52-63`: `sigaddset` computes `1u << (signo - 1)` on `uint32_t`.
Any signal number >32 (e.g., Linux-style numbering up to 31 + realtime 34-64) is
undefined behaviour. Currently only numbers up to 22 are used, but the header's comment
claims "MSVC appears to follow the Linux signal numbering" — with `SIGSYS`(31) this is
just inside the limit; realtime signals would overflow.

### 4.6 Missing `SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards

`thrd_signal_handle_posix.c.ipp:53-54` uses `SIGSYS`, and `:109` uses `SIGXCPU`/`SIGXFSZ`
without `#ifdef` guards (only `SIGPOLL` is guarded). On a POSIX platform that omits any
of these (they are all POSIX-required, but some embedded/BSD variants differ) the file
fails to compile. Windows provides all three, but the Windows backend does not use them.

### 4.7 `ucontext_t` and `siginfo_t` portability

`thrd_signal_handle.h:202-216` relies on `<signal.h>` defining `ucontext_t` (POSIX does
not require this; `<ucontext.h>` does) and uses platform-specific spellings
(`struct __siginfo`, `struct siginfo`) that assume BSD/Android/glibc layouts.

### 4.8 Mingw

Deliberately unsupported (`#error` at `thrd_signal_handle_windows.c.ipp:233-235`), but
note that before reaching that `#error` the header has already redefined `sigset_t` on
`_WIN32` (`thrd_signal_handle.h:43`), which collides with MinGW's own `sigset_t`
typedef in `<signal.h>` — the first of several Mingw incompatibilities.

### 4.9 `_setjmp` vs `setjmp` inconsistency for header-only consumers

`WG14_SIGNALS_HAVE__SETJMP` is set only on the compiled library target
(`CMakeLists.txt:32-34`, PRIVATE). Header-only consumers never get the definition and
always use `setjmp` (saving/restoring the signal mask) even when `_setjmp` is available.
Not a correctness bug but a silent performance/behaviour split between the two modes.

### 4.10 `__VA_OPT__` dependency

`SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:86`) requires `__VA_OPT__` (C23 /
C++20, or the GCC>=8 / Clang>=12 extension). Verified: GCC and Clang accept it silently
in C11 mode with `-Wpedantic` (no warning), so this works on current toolchains, but it
is a latent portability break for older compilers or strict MSVC C modes.

---

## 5. Build-system and packaging issues

### 5.1 `HEADER_ONLY_BUILD` option is broken (see 1.8)

### 5.2 Static library requires a C++ runtime, not declared

`thread_atexit.cpp` is always compiled into the library (C++). The CMake package
(`ProjectConfig.cmake.in`) does not express the C++ standard library dependency
(`stdc++`/`libc++`), so a plain C consumer that links `libwg14_signals.a` gets
unresolved C++ runtime symbols (verified: linking the test C program against the static
library requires `clang++`).

### 5.3 CMake `CMAKE_C_STANDARD` cache variable is unused for consumers and the header-only test lacks `-Werror`

The library compiles with `-Werror` (`CMakeLists.txt:47`) but the tests and the
`header_only_test` target do not (`test/CMakeLists.txt:12`), so warnings that would
break a strict build are invisible. The `sigfence` `__VA_OPT__` usage in tests is a
prime example.

### 5.4 CI gaps

- No CI matrix runs `-DHEADER_ONLY_BUILD=ON` (broken, see 1.8).
- No CI runs with `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` on Linux (the fallback
  path is only exercised on macOS).
- No CI exercises `stdc_raise(0, ...)` as a standalone setup call (crashes on Windows
  and on fallback platforms, see 1.3/1.4).
- No CI exercises `signal_decider_create` with a mixed installed/not-installed set
  (deadlocks / UAF, see 1.1/1.2).
- No CI runs the header-only test as a *C* program.
- The Windows CI runs with `-DCMAKE_C_STANDARD` in {11,17} but the matrix name says
  `standard` and MSVC ignores the C-standard option for `/experimental:c11atomics` in
  some versions.
- No CI runs the benchmark targets at all (`-E benchmark`), so the performance claims in
  the README are not verified.
- No CI tests C11 atomics with an actual stress/TSan build; the spinlock code paths
  (3.1) would benefit from TSAN.

### 5.5 `ProjectConfig.cmake.in` references non-existent export names

`cmake/ProjectConfig.cmake.in:6-11` conditionally includes
`@PROJECT_NAME@SlExports.cmake`/`@PROJECT_NAME@DlExports.cmake`, which are never
generated. Harmless (guarded by `EXISTS`), but misleading.

---

## 6. Test-quality issues

### 6.1 `WG14_SIGNALS_PREFIX(fn(args))` misuse (works only by accident)

`test/thrd_signal_handle_test.c:101,121`, `test/thrd_sigfpe_test.c:111`,
`test/benchmark_thrd_signal_handle_test.c:116` all write
`WG14_SIGNALS_PREFIX(signal_decider_destroy(sigill_decider))` — the prefix macro is
applied to the whole call expression. This only compiles because the default
`WG14_SIGNALS_PREFIX(x)` is identity; with any custom prefix the tests fail to build.
The correct spelling is `WG14_SIGNALS_PREFIX(signal_decider_destroy)(sigill_decider)`.

### 6.2 Test storage exhaustion

`test/async_signal_safe_tls_test.c:6-14` and `test/header_only_test.cpp:16-24`: `create`
does `*dest = storage_ptr++` on a 2-element array; any third `thread_init` (e.g., a
re-run of the main-thread init after the worker also inits, or the documented "safe to
call many times") writes out of bounds. The test also never verifies the re-init and
re-entrancy semantics documented in the API.

### 6.3 No coverage of the failure/edge APIs

No test calls `siguninstall_system`, `sigfillset_synchronous/asynchronous_*` return
values, `signal_decider_create` with NULL/empty guarded sets, or `sigguarded` with NULL
recovery (the POSIX NULL-recovery fall-through vs Windows livelock divergence, 1.7, is
untested).

### 6.4 The SIGFPE test depends on architecture trap behaviour

`test/thrd_sigfpe_test.c:62-67` works around x64's lack of integer-divide trapping, but
the guard relies on `sigfence` and `stdc_raise(SIGFPE)` fallback, so the "real fault"
SEH path on Windows (which is exactly the path with the 1.5/1.6/1.7 defects) is never
exercised.

---

## 7. Async-signal-safety claims vs reality

The public API makes strong claims ("ASYNC-SIGNAL-SAFE", "USUALLY ASYNC-SIGNAL-SAFE").
Verdicts:

- `tss_async_signal_safe_get`: NOT safe in the general case. It takes a spinlock
  (3.1), reads a cached thread ID (fine once populated), and on cache-miss performs
  async-signal-unsafe syscalls on Apple (4.3). Safe only when (a) the TLS cache was
  primed outside the handler and (b) no other thread/handler holds the object's lock.
- `current_thread_id`: safe on Linux/ELF (initial-exec TLS; the `gettid` syscall is
  signal-safe) and Windows; unsafe on Apple on first use per thread (4.3).
- `sigfillset_*`: safe (read-only static init with a benign double-checked write race —
  see 7.1).
- `sigguarded` / `stdc_raise`: "usually safe" only after the per-thread setup call, and
  only if the setup call itself is safe on the platform (it is not on Windows or
  fallback-TLS platforms — 1.3/1.4). On Linux with the documented pre-call, the happy
  path is signal-safe.
- `siginstall` / `siguninstall` / `signal_decider_create` / `signal_decider_destroy`:
  NOT async-signal-safe (malloc, fprintf, locks) — correctly documented as
  THREADSAFE-only.

### 7.1 Data race in the `sigfillset_*` lazy initialisation

`thrd_signal_handle_posix.c.ipp:49-67` double-checks `sigismember(&v, signos[0])` then
writes `v = x` without a lock or atomics. Two threads calling
`sigfillset_synchronous` concurrently both compute `x` and both store `v` — a benign
write-write race on identical values, but still a data race under the C memory model
(UB), and the "is it initialised?" check reads a non-atomic while another thread writes.
Trivially fixable with a static initialiser or C11 atomics; note also that if a real
signal is raised while `v` is half-written the reader can see a torn set (in practice
the sigset write is aligned and atomic).

### 7.2 `prepare_rsi` leaves `rsi->value` indeterminate on POSIX

`thrd_signal_handle_posix.c.ipp:186-199` does not initialise `rsi->value`; the global
decider loop always overwrites it (`:336`) and the frame path uses the frame's persistent
`rsi`, so no read of the indeterminate value occurs today — but any future code path
that inspects `rsi` before the decider loop would read garbage.

---

## 8. setjmp/longjmp correctness concerns

### 8.1 Modified-local-after-setjmp UB in POSIX `sigguarded`

`thrd_signal_handle_posix.c.ipp:240-266`:

```c
struct ... *old = tss->front, current;
memset(&current, 0, sizeof(current));
current.prev = old; ... current.rsi.value = value;
tss->front = &current;
if(WG14_SIGNALS_SETJMP(current.buf) != 0)
{
  tss->front = old;
  return recovery(&current.rsi);   /* reads `current.rsi` — modified by the handler
                                      after setjmp, and `current` is not volatile */
}
```

`current.rsi` is written by `prepare_rsi` (via the frame pointer in the signal handler,
i.e., after `setjmp` executed) and then read after `longjmp`. Per C11 7.13.2.1p3,
non-volatile automatic objects modified between `setjmp` and `longjmp` have
indeterminate values after `longjmp` — this is UB (works in practice on mainstream
compilers because the frame is a memory object, but a conforming compiler may cache
`current` in registers). The same applies to reading `current.guarded/recovery/decider`
after the longjmp (they are read indirectly via `frame->decider` in `stdc_raise`, which
runs *inside* the handler, so they are safe; only `rsi` is read post-longjmp). The
struct should be `volatile` (or the members accessed post-longjmp should be).

### 8.2 `_setjmp`/`setjmp` selection changes signal-mask semantics

With `setjmp` (used when `_setjmp` is unavailable) the mask saved at the `setjmp` is
restored on `longjmp` — combined with `SA_NODEFER` handlers this can silently
unblock/block signals relative to the interrupted context. Platform-dependent.

---

## 9. Minor issues and observations

- `siguninstall`'s `-1` failure path (`thrd_signal_handle_common.ipp.ipp:423-426`)
  leaks `ss`; and since `uninstall_sighandler` always returns `true`, the error path is
  dead code.
- `signal_decider_destroy` acquires `state->lock` per signal (NSIG iterations), even for
  signals not in the guarded set — O(NSIG) lock round-trips for a single destroy.
- `install_sighandler` never flushes `deferred_frees` (only uninstall/destroy do), so
  deferred nodes are held until an uninstall — a minor, documented-ish memory retention.
- `siginstall` returns the allocated `sigset_t *`; there is no way to uninstall by
  signal subset, and `siguninstall` frees the passed pointer, making double-uninstall a
  UAF (user error, undocumented).
- The `benchmark_thrd_signal_handle_test.c` uses `CHECK()` inside the timed loop (line
  103), which adds `fprintf` overhead to the measured critical path on failure only —
  negligible but non-idiomatic.
- `test_common.h:18-21` uses `__has_include(<threads.h>)` in C11 — fine on GCC/Clang.
- `config.h:122-130` opens and closes an empty `extern "C"` block — harmless but dead.
- `Readme.md:139` "Known bugs" section only lists the `pcpp` future work; none of the
  bugs above are listed.
- `doc/html/` is a committed Doxygen build output — version-controlled generated
  artifacts (churn, but not a bug).
- `build/` and `.vscode/` are untracked local artifacts — fine.

---

## 10. Priority-ordered remediation summary

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| 1.1 | ~~Critical~~ **FIXED** | `signal_decider_create` leaks global spinlock on non-installed signal (fixed at `thrd_signal_handle_common.ipp.ipp:513`) | `thrd_signal_handle_common.ipp.ipp:513` |
| 1.2 | ~~Critical~~ **FIXED** | decider-handle slot misalignment -> UAF on destroy + raise (fixed at `thrd_signal_handle_common.ipp.ipp:514`) | `thrd_signal_handle_common.ipp.ipp:443-614` |
| 1.3 | ~~Critical~~ **FIXED** | `sigguarded`/`stdc_raise` NULL-deref before first `siginstall` on fallback-TLS platforms (fixed at `thrd_signal_handle_common.ipp.ipp:264-272`) | `thrd_signal_handle_common.ipp.ipp:264-268`, `tss_async_signal_safe.c.ipp:177` |
| 1.4 | Critical | Windows `stdc_raise(0,...)` aborts | `thrd_signal_handle_windows.c.ipp:277-278` |
| 1.5 | High | Windows `prepare_rsi` OOB `ExceptionInformation` read | `thrd_signal_handle_windows.c.ipp:190-191` |
| 1.6 | High | Windows `tss->front` frame stack dangling/unmaintained | `thrd_signal_handle_windows.c.ipp:265-299,357-367` |
| 1.7 | High | Windows NULL-recovery + invoke_recovery -> infinite fault loop | `thrd_signal_handle_windows.c.ipp:210-213` |
| 1.8 | High | Header-only build broken (build option + C path) | CMake + all `.ipp` |
| 2.1 | High | tss deinit count/state race -> UAF | `tss_async_signal_safe.c.ipp:136-168` |
| 2.2 | High | `siguninstall` vs in-flight `stdc_raise` -> container UAF | `thrd_signal_handle_posix.c.ipp:314-368` |
| 2.3 | Med | `sighandlers_count` increment before TSS-create check | `thrd_signal_handle_common.ipp.ipp:316-323` |
| 2.4 | Med | Dead code after `return` in `sig_global_tss_state_destroy` | `thrd_signal_handle_common.ipp.ipp:276-281` |
| 2.5 | Med | `thread_init` returns success for NULL item | `tss_async_signal_safe.c.ipp:186-190` |
| 2.6 | Med | `tss_async_signal_safe_*` NULL handle crash | `tss_async_signal_safe.c.ipp:93-243` |
| 3.1 | Med | Spinlock not async-signal-safe | `lock_unlock.h` |
| 3.3 | Med | `SA_NOCLDWAIT`/`SA_NODEFER`/no `SA_RESTART` semantics | `thrd_signal_handle_posix.c.ipp:371-383` |
| 4.1 | Med | FreeBSD-only fallback for `get_current_thread_id` | `current_thread_id.c.ipp:70-72` |
| 8.1 | Low | Post-longjmp access to modified non-volatile locals | `thrd_signal_handle_posix.c.ipp:252-258` |

## 11. Methodology notes

- All library code (headers, `.ipp` files, sources, tests, CMake) read in full.
- Reproduced with live builds: normal CI config (passes), `HEADER_ONLY_BUILD=ON`
  (fails to compile), decider deadlock (1.1), decider UAF (1.2, via a patched overlay
  copy of the library to isolate from 1.1), and the `sigguarded`-without-`siginstall`
  crash (1.3).
- Windows findings (1.4-1.7, 4.4, 4.5) are code-level; they could not be executed on
  this host. The Windows CI build matrix passes for the code paths the tests exercise,
  which are exactly the paths these findings avoid.

---

## 12. Second review pass (2026-08-05, same revision): verification, corrections, new findings

### 12.1 Live verification results (macOS arm64, clang 17, CMake 4.3.2)

| First-pass claim | Result of re-verification |
|---|---|
| Normal CI build + tests | PASS: Debug build, `ctest -E benchmark` — 4/4 (tls, handle, sigfpe, header_only C++) |
| 1.1 decider lock leak | REPRODUCED: WARNING printed, next `stdc_raise` spins forever |
| 1.2 decider slot misalignment UAF | REPRODUCED (patched overlay fixing 1.1): `signal_decider_destroy` then `stdc_raise(SIGUSR2)` → SIGBUS (exit 138) |
| 1.3 `sigguarded` without `siginstall` | REPRODUCED: SIGSEGV (exit 139) at `tss_async_signal_safe_thread_init(NULL)` |
| 1.3b documented `stdc_raise(0, NULL, NULL)` setup call | REPRODUCED: SIGSEGV (exit 139) on fallback path (also aborts on Windows per 1.4) |
| 1.8 `-DHEADER_ONLY_BUILD=ON` | REPRODUCED: `redefinition of 'get_current_thread_id'` / `'internal_current_thread_id_cached_set'`, `-Werror,-Wstatic-in-inline` failures |
| 1.8 C-language header-only consumer | REPRODUCED: link fails — undefined `_thread_atexit` (single-TU and multi-TU at -O0); *undefined* symbols, not duplicates (see C3) |
| 5.2 C++ runtime dependency of static lib | REPRODUCED: linking a C program without `-lc++` → undefined libc++ exception symbols from `thread_atexit.cpp.o` |
| **NEW V1** installed package usable? | **FAILS**: `cmake --install` ships no headers (only `libwg14_signals.a` + CMake config files), and `find_package(wg14_signals REQUIRED)` hard-errors ("Unknown CMake command `check_required_components`") |

### 12.2 Corrections to first-pass claims

- **C1 (fixes 1.7):** POSIX `stdc_raise` `sig_decision_invoke_recovery` with a NULL
  `recovery` does NOT fall through to the next decider or the previous handler — it
  executes `return true` (`thrd_signal_handle_posix.c.ipp:302-307`). For a *real*
  hardware fault the handler then returns and the faulting instruction re-executes:
  the POSIX backend livelocks exactly like Windows. Both backends need a fix here
  (POSIX should continue to outer frames / the previous handler instead of `return
  true`).
- **C2 (fixes 1.6):** the "real fault inside a `sigguarded` region, global decider
  returns resume" infinite-fault-loop is not Windows-specific: on POSIX the same
  sequence makes `stdc_raise` return true, the handler return, and the faulting
  instruction re-fault forever. The backend divergence is only in *which* deciders run
  first (frames vs global), not in whether a livelock occurs.
- **C3 (fixes 1.8):** C header-only consumers fail with *undefined symbols*, not
  duplicate-symbol link errors. C11 6.7.4p7: when every file-scope declaration carries
  `inline`, the .ipp definitions are inline definitions and emit no external symbol;
  any call that is not inlined (cross-TU, or -O0) becomes an undefined reference, and
  `thread_atexit` has no C implementation at all. Duplicate symbols would require a
  non-inline external definition somewhere, which does not exist.
- **C4 (refines 1.5):** the `ExceptionInformation[2]`/`[1]` reads stay within the fixed
  15-slot array, so this is not a heap overrun — but they read past the valid prefix
  (`NumberParameters`) for *every* genuine access violation (which has 2 parameters),
  giving deciders an indeterminate "NTSTATUS" on virtually every real fault, plus
  indeterminate `addr` for 0-parameter exceptions (e.g. `EXCEPTION_INT_DIVIDE_BY_ZERO`).
- **C5 (clarifies 2.2):** the decider-node refcount protocol itself is sound (a raise's
  `refcount++` always precedes the unlocked decider call, so a concurrent destroy can
  never free the node mid-call); the 2.2 UAF is strictly the container
  (`sighandler_info` freed by `siguninstall` while `stdc_raise` still holds `it` and
  re-accesses `value(it)->global_handler` after re-locking).

### 12.3 New findings

#### V1 [verified] Installed package is unusable: no headers installed, `find_package` hard-fails (Critical, packaging)

`CMakeLists.txt` has no `install(DIRECTORY include/ ...)` rule — `cmake --install`
produces only `lib/libwg14_signals.a` and `lib/cmake/wg14_signals/*`. Additionally,
`ProjectConfig.cmake.in` is processed with plain `configure_file(... @ONLY)` instead of
`configure_package_config_file()`, so `@PACKAGE_INIT@` / `@PROJECT_PACKAGE_DEPENDENCIES@`
never receive their definitions:

- CMake >= 4.0 (`configure_file` now substitutes *undefined* `@VAR@` with empty
  strings — verified on 4.3.2) → the installed config loses the `PACKAGE_INIT` macro
  block and `check_required_components(wg14_signals)` at line 13 errors out →
  `find_package(wg14_signals REQUIRED)` fails with "Unknown CMake command".
- CMake < 4.0 → the literal `@PACKAGE_INIT@` remains in the installed file and fails as
  an unknown command on include.

Either way the documented consumption path is broken, and even after fixing the config
the exported target's `INTERFACE_INCLUDE_DIRECTORIES` points at
`<prefix>/include`, which does not exist. First pass (5.5) only noted the harmless
`SlExports`/`DlExports` references and missed this.

#### V2 [code-level, Windows] `win32_vectored_exception_function` NULL-derefs the per-thread state on fresh threads (High)

`thrd_signal_handle_windows.c.ipp:357-361`: when a global decider returns a claiming
decision, the handler calls `sig_global_tss_state()` and immediately dereferences
`tss->front`. The per-thread TLS state is created only by `sig_global_tss_state_init()`
(i.e. a prior `sigguarded`/`stdc_raise` on that thread); the vectored handler never
initialises it. A genuine fault (AV, div-by-zero) on a thread that has only ever called
`siginstall` (or nothing) → `tss == NULL` → NULL dereference *inside the exception
handler*, turning a recoverable fault into a crash (or a crash-loop as the handler
faults again). POSIX is immune (its handler inits the TLS state as part of the raise).
The Windows tests never hit it because every test thread calls `stdc_raise` before any
fault can reach the vectored handler.

#### V3 [code-level, Windows] Unsupported exception codes reach `sigismember(guarded, 0)` → UB; C++ exceptions can be swallowed by `sigguarded` (High)

`win32_exception_filter` (`:194-217`) evaluates `sigismember(guarded, signo)` where
`signo = signal_from_win32_exception_code(GetExceptionCode())` — 0 for every unsupported
code (C++ exceptions `0xE06D7363`, third-party `RaiseException`s, CRT codes). The
Windows inline `sigismember` computes `1u << (signo - 1)` = `1u << -1`: undefined
behaviour (masked to bit 31 on x86/ARM64). Consequences for a `sigfillset`-built guard
set (bit 31 set):

- the guard claims "signal 0", the user decider runs with a garbage `rsi` (signo 0,
  indeterminate `error_code`/`addr`);
- if the decider returns `sig_decision_invoke_recovery` with a recovery function, the
  `__except` body runs and **the foreign exception is swallowed**. In C++ this breaks
  exception semantics: an exception thrown inside `sigguarded` (e.g. `std::bad_alloc`)
  is caught by the `__except` block instead of propagating — the C++ unwinder never
  runs. Any decider that unconditionally returns `invoke_recovery` (as every test
  decider does) triggers this. The filter must range-check `signo >= 1` before the
  membership test.

#### V4 [code-level, Windows] `stdc_raise` aborts for every unsupported signo, not just 0 (High — scope extension of 1.4)

`win32_exception_code_from_signal` handles only SIGABRT/SIGBUS/SIGILL/SIGSEGV/SIGFPE;
`stdc_raise(SIGINT)`, `stdc_raise(SIGTERM)`, `stdc_raise(SIGPIPE)`, `stdc_raise(SIGUSR1)`
etc. all hit `default: abort()`. On POSIX the same calls are harmless no-ops when no
decider is installed. The header documents `stdc_raise` as usable for "OUR currently
installed signal decider" for arbitrary signals.

#### V5 [code-level, Windows] Global deciders can be invoked two or three times per single exception (Medium)

The same function is registered both as `AddVectoredContinueHandler` and as the
unhandled exception filter (`install_sighandler_impl`, `:415-424`). For an exception not
claimed by the frame `__except` filter, the dispatch order is: vectored handler
(global deciders) → frame filters (frame decider) → `UnhandledExceptionFilter` (global
deciders **again**) → vectored continue handlers. Any side-effecting decider (counters,
state fixes) therefore runs twice for one fault on the no-debugger path; under a
debugger the continue handler runs again too. POSIX invokes each decider exactly once
per raise. The in-code comment documents the order but not the double invocation.

#### V6 [code-level, both backends] Frame published before `setjmp` completes → longjmp into an uninitialised buffer (Medium)

POSIX `sigguarded` (`thrd_signal_handle_posix.c.ipp:251-252`) and Windows `stdc_raise`
(`thrd_signal_handle_windows.c.ipp:270-271`) both execute `tss->front = &current`
before `setjmp(current.buf)` returns. A signal/exception delivered in that instruction
window runs `stdc_raise`/the vectored handler, which may `longjmp(current.buf)` before
`setjmp` has stored the environment — undefined behaviour (crash or longjmp to garbage).
The window is a few instructions wide but is exactly the async nature the API claims to
handle; a `sigsetjmp`-style block/unblock or a two-phase publish would close it.

#### V7 [code-level, fallback path] `siguninstall` of the last handler while another thread is inside `sigguarded` frees that thread's live state (Medium)

On the fallback (Apple) path, the final `siguninstall` → `sig_global_tss_state_destroy`
→ `tss_async_signal_safe_destroy` frees the shared TSS (and every per-thread entry)
*while another thread may be between `sigguarded` frames*. The interrupted thread's
`tss->front` points into the freed per-thread state; its next raise runs
`sig_global_tss_state_init` against the *dangling* `*sig_tss_state_raw()` (the 2.4 dead
code never resets it) → UAF, and its frame pop (`tss->front = old`) writes into freed
memory. Only the fallback path is affected (async-safe TLS path's destroy is a no-op).
The usage requirement "destroy only after all threads have left `sigguarded`" is
nowhere documented.

#### V8 [code-level, Windows] `stdc_raise(SIGABRT)` raises a non-continuable exception; "resume" from a decider loops (Low-Medium)

`SIGABRT` maps to `EXCEPTION_NONCONTINUABLE_EXCEPTION (0xC0000025)`. If any decider
returns "resume execution" for it, Windows re-raises `0xC0000025` (the OS cannot resume
a non-continuable exception), the vectored handler runs the decider again → repeat loop.
POSIX has no such constraint for SIGABRT. (With no decider and an enclosing `__except`
the raise still works, which is why tests pass.)

### 12.4 Minor additions

- **M1:** `siguninstall` unconditionally `free()`s the pointer it is handed
  (`thrd_signal_handle_common.ipp.ipp:429`). The docs never state that the pointer must
  be the exact value returned by `siginstall`; passing a user-owned sigset (stack
  object) → invalid free. The free-by-design also makes double-uninstall a UAF (extends
  §9).
- **M2:** on the async-safe TLS path, `sig_global_tss_state_init` sets `*state = mem`
  *before* calling `thread_atexit(free, mem)`; if registration fails, the function
  returns -1 but the pointer stays set, so the next call silently succeeds with a
  leaked `mem` that will never be freed at thread exit (sibling of 3.14).
- **M3:** `sigguarded` failure returns `ret.int_value = -1`, which is
  indistinguishable from a guarded function legitimately returning -1; no error
  return is documented for `sigguarded`.
- **M4:** `synchronous_sigset`/`asynchronous_*_sigset` carry
  `__attribute__((constructor))` in addition to being lazy-initialised; in practice the
  load-time constructor pre-initialises the statics, which substantially mitigates the
  7.1 write-write race for executables (shared libraries loaded at runtime are still
  racy). The attribute is harmless but appears unintended on a value-returning function.

### 12.5 Updated priority-ordered remediation additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| V1 | Critical | Installed package: no headers installed; `find_package` fails (PACKAGE_INIT never expanded) | `CMakeLists.txt:50-70`, `cmake/ProjectConfig.cmake.in` |
| V2 | High | Windows vectored handler NULL-derefs `tss->front` on fresh threads | `thrd_signal_handle_windows.c.ipp:357-361` |
| V3 | High | Windows `sigismember(guarded, 0)` UB; C++ exceptions swallowed by `sigguarded` | `thrd_signal_handle_windows.c.ipp:200`, `thrd_signal_handle.h:52-63` |
| V4 | High | Windows `stdc_raise` aborts for all unsupported signos (extends 1.4) | `thrd_signal_handle_windows.c.ipp:112-134` |
| C1 | Med | POSIX NULL-recovery + invoke_recovery returns true → re-fault livelock (1.7 correction) | `thrd_signal_handle_posix.c.ipp:302-307` |
| V5 | Med | Windows global deciders run 2-3x per exception | `thrd_signal_handle_windows.c.ipp:408-427` |
| V6 | Med | setjmp-buffer race: frame published before setjmp completes | `thrd_signal_handle_posix.c.ipp:251-252`, `thrd_signal_handle_windows.c.ipp:270-271` |
| V7 | Med | `siguninstall` during another thread's `sigguarded` frees live TSS (fallback path) | `thrd_signal_handle_common.ipp.ipp:348-360` |
| V8 | Low | `stdc_raise(SIGABRT)` non-continuable exception: resume loops | `thrd_signal_handle_windows.c.ipp:118-120` |

### 12.6 Verification artifacts

Repro programs (temporary): `repro11.c` (1.1 hang), `repro12.c` (1.2 UAF, against a
one-line-patched overlay copy), `repro13.c` (1.3 `sigguarded` crash), `repro_setup.c`
(1.3b `stdc_raise(0,...)` crash), plus a `find_package` consumer project against a
`cmake --install` tree (V1). All were compiled with `-std=c11` against the built static
library on macOS arm64 and produced the documented outcomes.

---

## 13. Third review pass (2026-08-06, same revision): re-verification, new findings, corrections

Same revision as passes 1-2 (`f48e95e`) plus the uncommitted `config.h` whitespace and
`nullptr`-for-C++ change. Every header, every `.ipp`, every source file, both backends,
all tests, CMake, and the CI matrix were re-read in full; all control-flow paths in
`tss_async_signal_safe`, the common signal-handle core, and both backends were traced
again, with particular attention to error paths, re-entrancy, and OOM windows.

### 13.1 Live verification (macOS arm64, clang 17, CMake 4.3.2, sanitize toolchain)

| Check | Result |
|---|---|
| Rebuild of current tree (RelWithDebInfo + ASan/UBSan) | PASS (20/20 targets) |
| `ctest -E benchmark` | 4/4 PASS (tls, handle, sigfpe, header_only C++) |
| **NEW W2** (below): global decider receives indeterminate `error_code`/`addr`/`raw_info` for `stdc_raise(signo, NULL, NULL)` | REPRODUCED: `error_code=2`, `addr=0xce00985fe612009b`, `raw_info=<heap>` on two consecutive raises of SIGUSR1 with NULL `info` — all three fields are stack garbage, not the documented `si_errno`/`si_addr`/`siginfo_t*` |
| Prior pass claims 1.1-1.8, V1-V8 | Unchanged by the `config.h` delta (code paths untouched); no re-runs performed for already-reproduced items |

### 13.2 New findings

#### W1 [code-level, fallback TLS path] `tss_async_signal_safe_thread_init` leaves a dangling map entry when the `deinit_state` allocation fails (High — OOM corner)

`tss_async_signal_safe.c.ipp:201-213`. When `calloc` of `mem->state` (the
`deinit_state`) fails:

```c
mem->state = calloc(1, sizeof(deinit_state));
if(mem->state == NULL)
{
  UNLOCK(mem->lock);
  mem->attr.destroy(newitem);   /* newitem freed ...            */
  errno = ENOMEM;
  return -1;                    /* ... but the map entry [mytid]
                                   -> newitem is NOT erased     */
}
```

The `[mytid] → newitem` entry was committed at line 192-193 *before* the `state`
allocation, and the failure path destroys `newitem` without erasing the entry. The map
now holds a dangling pointer:

- The next `tss_async_signal_safe_get` on that thread returns the freed pointer
  (use-after-free on read).
- The next `tss_async_signal_safe_thread_init` on that thread finds the entry, skips
  the create, and reports success (0) — the thread is permanently bound to freed
  storage, `count` is never incremented for it, and no atexit cleanup is registered.

The sibling failure path above it (`insert` returning end, lines 194-200) is correct
(no entry was committed). Only the `deinit_state`-OOM path is broken. Fix: erase the
map entry before destroying `newitem` (or move the `state` allocation before the
`insert`).

#### W2 [verified, POSIX] deciders receive indeterminate/stale `error_code`, `addr`, `raw_info` for `stdc_raise(signo, NULL, NULL)` (Medium-High)

`prepare_rsi` (`thrd_signal_handle_posix.c.ipp:186-199`) writes `raw_info`,
`error_code`, and `addr` **only** when `siginfo != NULL`:

- Global path: `rsi` is a fresh uninitialised stack variable
  (`thrd_signal_handle_posix.c.ipp:327`) — all three fields are indeterminate garbage
  (reproduced, see 13.1). The documented usage `stdc_raise(signo, nullptr, nullptr)`
  is exactly what the tests and README use, so any decider reading
  `rsi->error_code`/`addr`/`raw_info` (the API invites this: the header documents
  them as "the `si_errno` code" and "memory location which caused fault") reads
  garbage — and `raw_info` is a garbage *pointer* that the decider may dereference.
- Frame path: the frame's `rsi` is zeroed once at `sigguarded` entry, so the first
  raise sees zeroed fields, but any *second* raise in the same frame with NULL
  `info` keeps the **stale** `raw_info` pointer from the first raise — a pointer into
  a dead kernel stack frame — plus stale `error_code`/`addr`.

Pass 2's 7.2 only noted `value`; the defect is broader. Fix: memset the struct (or
explicitly set the three fields) in `prepare_rsi` regardless of `siginfo`.

#### W3 [code-level, POSIX] nested signal delivery during a decider call races on the shared frame `rsi` (Medium)

`thrd_signal_handle_posix.c.ipp:294-295` — the frame's `rsi` is both written
(`prepare_rsi`) and read (`frame->decider(&frame->rsi)`) from the same thread. With
`SA_NODEFER` (which `install_sighandler_impl` always sets), a second delivery of a
guarded signal while the first decider is still executing re-enters
`raw_signal_handler` → `stdc_raise` → `prepare_rsi` on the **same** `frame->rsi`,
overwriting `signo`/`error_code`/`addr`/`raw_info` mid-decider:

- The outer decider reads torn/replaced fields after the nested delivery completes.
- If the nested raise chooses `invoke_recovery`, it `longjmp`s out of the outer
  decider entirely — the outer decider is abandoned mid-flight and the *outer*
  recovery runs with the *nested* `rsi` contents.

Not a C memory-model race (same thread), but a re-entrancy aliasing bug; the two
backends differ: on Windows the frame `rsi` is a fresh local in
`win32_exception_filter`, so nested exceptions cannot corrupt it (they can only
longjmp away from it).

#### W4 [code-level, Windows] the 2.2 container UAF exists in the Windows vectored handler too (High)

Pass 1's 2.2 documented the `siguninstall` vs. in-flight `stdc_raise` container
use-after-free for the POSIX `stdc_raise` only. The identical pattern exists in
`win32_vectored_exception_function` (`thrd_signal_handle_windows.c.ipp:329-368`): the
handler holds `it` across the unlocked `current->decider(&rsi)` call and re-accesses
`signo_to_sighandler_map_t_value(it)->global_handler` after re-locking. A concurrent
`siguninstall` that drops the last reference frees the `sighandler_info` container in
that window → UAF *inside the exception handler*, turning a recoverable fault into a
crash. Additionally the vectored handler may be *removed* (`RemoveVectoredContinueHandler`)
mid-raise when the count reaches zero, so the remainder of the raise runs with no
library filter at all.

#### W5 [code-level, Windows] `stdc_raise` can never return `false`; an unclaimed raise terminates the process (High — documented-contract violation)

`thrd_signal_handle_windows.c.ipp:252-300` raises via `RaiseException` and always
returns `true`. The header contract (`thrd_signal_handle.h:362-364`) promises
"returning false if we have no decider installed for that signal". On Windows:

- Supported signo, no installed handler and no guard: `RaiseException` → no vectored
  handler, no frame filter → the registered `UnhandledExceptionFilter` (the library's
  own function, which returns `EXCEPTION_CONTINUE_SEARCH`) → Windows Error Reporting
  **terminates the process**. No `false` is ever returned.
- Supported signo inside a `sigguarded` frame guarding a *different* signal: same
  outcome — the filter returns `EXCEPTION_CONTINUE_SEARCH` because the signal is not
  in the guard set, and the process dies instead of the raise returning `false`.

So the portable error-handling idiom `if(!stdc_raise(signo, ...)) { /* fall back */ }`
is deadly on Windows. This is distinct from 1.4/V4 (which cover `abort()` on
unsupported signos); this is the "no decider" path for *supported* signos, which on
POSIX returns `false` cleanly.

#### W6 [build] `PROJECT_IS_TOP_LEVEL` requires CMake ≥ 3.21 while `cmake_minimum_required` is 3.15 (Low-Medium)

`CMakeLists.txt:1` declares `cmake_minimum_required(VERSION 3.15 FATAL_ERROR)`, but
line 72 gates the entire `test/` subdirectory on `PROJECT_IS_TOP_LEVEL`, a variable
introduced in CMake 3.21. On CMake 3.15-3.20 the variable is undefined and the
condition silently evaluates false: no tests, no `header_only_test` target, and the
`BUILD_TESTING` option is ignored — with no diagnostic. Either raise the minimum to
3.21 or use `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`.

#### W7 [build, minor] MSVC builds lack the `-Werror` equivalent

`CMakeLists.txt:44-48`: GCC/Clang get `-Wall -Wextra -Wpedantic -Werror`; MSVC gets
`/W4 /experimental:c11atomics` with no `/WX`. All warnings that would break a strict
GCC/Clang build are therefore invisible in the Windows CI leg (extends 5.3, which only
noted the tests).

#### W8 [code-level, Windows, minor] `stdc_raise` mutates the caller's `EXCEPTION_RECORD`

`thrd_signal_handle_windows.c.ipp:282-293`: when `info != NULL` and room remains, the
function appends the `0xdeadbeefdeadbeef` marker and the raw context into
`info->ExceptionInformation[]` and bumps `info->NumberParameters` — mutating the
caller's record in place. If the caller passes a kernel-supplied `EXCEPTION_RECORD`
(re-raising a genuine fault from inside a filter/vectored handler — exactly the
"pass on signal handling to this library" use case documented in the header), the
record the kernel will later inspect is altered. The marker write is also racy if two
threads re-raise through the same record.

#### W9 [C++ consumers, minor] longjmp across objects with non-trivial destructors is UB

`stdc_raise`'s `invoke_recovery` path (`thrd_signal_handle_posix.c.ipp:308`) and the
Windows vectored handler's `longjmp` (`thrd_signal_handle_windows.c.ipp:363`) skip C++
destructors for any automatic object live in the guarded frame — UB per the C++
standard. The library is documented as C++-usable (the header-only test is C++), and
MSVC explicitly disables warning 4611 (`thrd_signal_handle_windows.c.ipp:46`) for it.
A C++ `sigguarded` caller with RAII objects in scope of a raised signal gets skipped
destructors silently.

#### W10 [minor] `signal_decider_destroy` double-destroy is an unguarded use-after-free

`thrd_signal_handle_common.ipp.ipp:612` (`free(p)`) unconditionally frees the handle.
A second `signal_decider_destroy` on the same pointer reads freed memory (the sigset
iteration at line 564 and the slot reads) before the double-free — same class as the
`siguninstall` double-free noted in M1, but for the decider handle. No guard exists.

#### W11 [C header-only, minor] `sigfence_force_escaped` is declared but never defined for C header-only consumers on non-GNU compilers

`thrd_signal_handle.h:78-79` declares `sigfence_force_escaped` (as `inline` in
header-only mode) and the fallback `SIGFENCE_IMPL_*` macros (`:135-153`) call it, but
the definition lives only in `src/wg14_signals/sigfence_force_escaped.c` — no header
pulls in `sigfence_force_escaped.c.ipp` in header-only mode. GNU-family compilers hide
this (the inline-asm path is used), so only a non-GNU C header-only consumer (e.g.
MSVC C mode, which takes the fallback path) that actually uses `sigfence(...)` fails
to link. Extends 1.8/C3.

### 13.3 Corrections and extensions to earlier passes

- **C6 (extends 2.2):** the `siguninstall`-vs-in-flight-raise container UAF is not
  POSIX-only; the Windows vectored handler has the identical pattern (W4), with the
  added twist that the filter can be removed mid-raise.
- **C7 (extends 6.2):** the 2-element `storage` exhaustion in the tests also applies
  to `test/benchmark_async_signal_safe_tls_test.c:12-13` (third `thread_init` writes
  out of bounds), not just `async_signal_safe_tls_test.c` and `header_only_test.cpp`.
- **C8 (extends 7.2):** the POSIX `prepare_rsi` indeterminacy is not limited to
  `value` — `error_code`, `addr`, and `raw_info` are indeterminate (global path) or
  stale (frame path, repeat raises) whenever `info == NULL` (W2, reproduced).
- **C9 (refines M4):** the `__attribute__((constructor))` mitigation for the 7.1
  lazy-init race is POSIX-only; the Windows `synchronous_sigset`/
  `asynchronous_nondebug_sigset` carry no constructor attribute, and Windows
  `asynchronous_debug_sigset` re-writes its static unconditionally on every call
  (benign, but the 7.1 race is unmitigated on Windows for the first two).
- **C10 (notes on 3.4):** the "reset to SIG_DFL and `pthread_kill(self)`" default path
  in `invoke_sigaction` also permanently discards the library's handler for
  stop/continue signals (SIGTSTP/SIGTTIN/SIGTTOU/SIGCONT) — after the process resumes
  from a stop, the map still claims the signal installed while the kernel handler is
  now `SIG_DFL`, so subsequent raises of that signal bypass the library entirely
  until a re-install.

### 13.4 Priority-ordered additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| W1 | High | dangling map entry after `deinit_state` OOM; next get/init returns freed pointer | `tss_async_signal_safe.c.ipp:201-213` |
| W2 | High | deciders get indeterminate/stale `error_code`/`addr`/`raw_info` for `stdc_raise(signo,NULL,NULL)` (reproduced) | `thrd_signal_handle_posix.c.ipp:186-199,327` |
| W4 | High | 2.2 container UAF also in Windows vectored handler; filter removable mid-raise | `thrd_signal_handle_windows.c.ipp:329-368` |
| W5 | High | Windows `stdc_raise` never returns false; unclaimed raises kill the process via WER | `thrd_signal_handle_windows.c.ipp:252-300` |
| W3 | Med | nested delivery overwrites the frame `rsi` mid-decider (SA_NODEFER re-entrancy) | `thrd_signal_handle_posix.c.ipp:294-295` |
| W6 | Med | `PROJECT_IS_TOP_LEVEL` needs CMake ≥ 3.21 vs `minimum_required(3.15)` — tests silently skipped | `CMakeLists.txt:1,72` |
| W7 | Low | MSVC library build lacks `/WX` | `CMakeLists.txt:44-48` |
| W8 | Low | `stdc_raise` mutates caller's `EXCEPTION_RECORD` in place | `thrd_signal_handle_windows.c.ipp:282-293` |
| W9 | Low | longjmp skips C++ destructors in guarded frames (UB; 4611 disabled) | `thrd_signal_handle_posix.c.ipp:308`, `thrd_signal_handle_windows.c.ipp:363` |
| W10 | Low | `signal_decider_destroy` double-destroy = unguarded UAF | `thrd_signal_handle_common.ipp.ipp:612` |
| W11 | Low | C header-only + non-GNU: `sigfence_force_escaped` declared, never defined | `thrd_signal_handle.h:78-79,135-153` |

### 13.5 Verification artifacts

- `repro_n3.c` (temporary): global decider + `stdc_raise(SIGUSR1, NULL, NULL)` twice
  — printed `error_code=2`, garbage `addr`, heap-garbage `raw_info` on both calls
  (W2). Compiled `-std=c11` with the sanitized static library on macOS arm64.
- Rebuild + full non-benchmark ctest suite of the current tree: 4/4 pass (13.1).

---

## 14. Fourth review pass (2026-08-06, same revision): live verification, corrections, new findings

Same revision as passes 1-3 (`f48e95e`) plus the uncommitted `config.h` delta. Every
header, every `.ipp`, every source file, all tests, all CMake files, the vendored
`verstable.h`, and the CI matrix were re-read in full. All control-flow paths in
`tss_async_signal_safe`, the common core, and both backends were re-traced, with
particular attention to paths the tests never exercise (error returns, OOM windows,
fresh-thread first use, post-uninstall use, reuse-after-destroy). The vendored
`verstable.h` instantiation was inspected for the two template uses
(`thread_id_to_tls_map_t`, `signo_to_sighandler_map_t` on `NSIG >= 1024` platforms); no
new issues were found in the vendored library itself beyond those noted in pass 1.

### 14.1 Live verification results (macOS arm64, clang 17, CMake 4.3.2)

| Check | Result |
|---|---|
| Baseline rebuild (Debug + ASan/UBSan sanitize toolchain, `C_STANDARD=11`, static) | PASS — 20/20 targets |
| `ctest` full suite (including the benchmark targets, which CI excludes) | 6/6 PASS (tls, handle, sigfpe, header_only C++, 2 benchmarks) |
| CI variant `BUILD_SHARED_LIBS=ON` + `CMAKE_C_STANDARD=23` | PASS — 4/4 non-benchmark tests |
| 1.1 decider lock leak | REPRODUCED: WARNING printed, process hangs in the next library call (kill -9 after 3 s) |
| 1.2 decider slot misalignment UAF | REPRODUCED with a *properly rebuilt* patched tree (see note below): `signal_decider_destroy` returns -1 while freeing the still-linked node; the next `stdc_raise(SIGUSR2)` is an ASan heap-use-after-free at `thrd_signal_handle_posix.c.ipp:336` (`rsi.value = current->value`) |
| 1.3 / 1.3b `sigguarded` / `stdc_raise(0, NULL, NULL)` without `siginstall` | REPRODUCED: SIGSEGV at `tss_async_signal_safe.c.ipp:177` in both cases |
| C1 NULL-`recovery` + `invoke_recovery` on a genuine fault (POSIX) | REPRODUCED: infinite re-fault livelock (process still running after 2 s; killed). Note: `__builtin_trap()` delivers SIGTRAP on arm64, so the repro guards SIGTRAP, not SIGILL |
| W2 indeterminate `error_code`/`addr`/`raw_info` | REPRODUCED: `error_code=0` (coincidence of ASan-poisoned stack), `addr`/`raw_info` = live stack garbage pointers |
| 1.8 `-DHEADER_ONLY_BUILD=ON` | REPRODUCED: `redefinition of 'get_current_thread_id'/'internal_current_thread_id_cached_set'` plus multiple `-Werror,-Wstatic-in-inline` failures |
| 1.8 C-language header-only consumer | REPRODUCED: link fails — `thread_atexit` is `inline`-declared but never defined (`-Wundefined-inline`), undefined reference |
| V1 packaging | REPRODUCED: `cmake --install` ships no headers; installed `wg14_signalsConfig.cmake` contains a bare `check_required_components(wg14_signals)` (PACKAGE_INIT never expanded) → `find_package(wg14_signals REQUIRED)` hard-errors |
| 2.8 `sigfence(<rvalue>)` | REPRODUCED: `error: invalid lvalue in asm output` (also verified `sigfence()` with zero args compiles) |
| 6.1 "prefix misuse breaks tests" claim | **REFUTED** — see C11 below: library and all four C tests compile and link with `WG14_SIGNALS_PREFIX(x)=foo_##x` |
| **NEW X1** `thread_init` after all registered threads exited | REPRODUCED: ASan heap-use-after-free (WRITE, `tss_async_signal_safe.c.ipp:214`) |
| **NEW X2** `destroy` after all registered threads exited | REPRODUCED: ASan heap-use-after-free (WRITE, `tss_async_signal_safe.c.ipp:119`) |

**Verification note (methodology):** the first attempt at reproducing 1.2 compiled the
repro with `-I <patched include tree>` but linked the *unpatched* static library — the
implementation comes from the library's `.o` files, so the patch was silently ignored
and the process still deadlocked. The reproduction only became valid after rebuilding
the library itself from the patched tree. The same trap applies to any header-level
"fix" verified against the prebuilt library.

### 14.2 New findings

#### X1 [verified, High] `tss_async_signal_safe_thread_init` performs a use-after-free on the `deinit_state` when called after all previously registered threads have exited

`tss_async_signal_safe.c.ipp:162-166` — the last thread's deinit runs
`free(state)` on the shared `deinit_state`, but **nothing clears `mem->state`**. A later
`thread_init` on a fresh thread (a completely ordinary sequential pattern: create →
T1 inits+exits → T2 inits) finds `mem->state` still non-NULL (dangling), skips the
allocation at `:201-204`, and executes `atomic_fetch_add(&mem->state->count, 1)` at
`:214` on freed memory:

- Reproduced with ASan: `heap-use-after-free` WRITE of size 4 at
  `tss_async_signal_safe.c.ipp:214`, allocation/free both in `thread_init`/`thread_deinit`
  of a previously exited thread (see 14.1).
- Second-order cascade: the `thread_atexit` registration then references the freed
  `state`, so the new thread's own exit runs `tss_async_signal_safe_thread_deinit` on
  freed memory again (`:140`, `state->val` read), plus a second `fetch_sub`/`free` on
  the same block.
- Pass 2's 2.1 documented a *concurrent* deinit race; X1 is a purely *sequential*
  lifetime bug in the same field — no threads need to overlap at all. The fallback TLS
  path (`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL == 0`, i.e. macOS/Apple and any
  non-GNU/MSVC platform) is affected; the async-safe TLS path never uses this object.

Fix direction: the last deinit (the one that frees `state`) must clear `mem->state`
(under `mem->lock`) before `free(state)` — it has `mem` via `state->val` — or
`thread_init` must validate `mem->state` against a generation/refcount.

#### X2 [verified, High] `tss_async_signal_safe_destroy` writes through the dangling `mem->state` when all registered threads exited before destroy

Same root cause as X1, different trigger. When the last registered thread has already
exited (so `state` was freed at `tss_async_signal_safe.c.ipp:165`), a later
`tss_async_signal_safe_destroy(val)` executes:

```c
if(mem->state)                /* dangling non-NULL */
{
  mem->state->val = NULL;     /* :119 — heap-use-after-free WRITE */
  mem->state = NULL;
}
```

Reproduced with ASan (see 14.1): WRITE of size 8 at `:119`, freed by the earlier
thread's deinit at `:165`. Note the documented destroy-after-join pattern ("destroy
only after all threads left") is exactly the pattern that hits this — the state is
already freed by the time destroy runs. The same dangling `mem->state` is then also
read by any later `thread_init` (X1). Both X1 and X2 are reachable through the public
`tss_async_signal_safe_*` API alone, with no library-internal involvement.

#### X3 [code-level, Windows, Medium-High] `sigguarded` on Windows never initialises the per-thread TSS (unlike POSIX)

`thrd_signal_handle_windows.c.ipp:219-249` — the Windows `sigguarded` performs no
`sig_global_tss_state_init()` call; POSIX does (`thrd_signal_handle_posix.c.ipp:234`).
A Windows thread whose only library interaction is `sigguarded()` (never `stdc_raise`)
has a NULL per-thread `sig_global_state_tss_state_t`. If a genuine fault then occurs
on that thread and a global decider claims it, the vectored handler evaluates
`tss->front` on the NULL state (`thrd_signal_handle_windows.c.ipp:357-361`) — a crash
*inside the exception handler*, turning a recoverable fault into a crash. This extends
pass 2's V2 (which assumed a thread that had never called any library function); X3
shows `sigguarded` alone is insufficient on Windows while being sufficient on POSIX —
a silent cross-platform behavioural divergence in the same API.

#### X4 [code-level, Windows, Medium] `EXCEPTION_STACK_OVERFLOW` (0xC00000FD) is not mapped to any signal

`signal_from_win32_exception_code` (`thrd_signal_handle_windows.c.ipp:135-164`) covers
the five raised signals plus the eight `EXCEPTION_FLT_*`/`EXCEPTION_INT_*` codes, but
has no case for `EXCEPTION_STACK_OVERFLOW`. A genuine stack overflow on Windows returns
`signo == 0` → `EXCEPTION_CONTINUE_SEARCH` → WER terminates the process with no
library involvement, whereas on POSIX a stack overflow delivers `SIGSEGV`, which *is*
handled (it is in the synchronous set and is installed by `siginstall(NULL)`). This is
a real functional gap between backends for a common, otherwise-recoverable fault
class, and it is not documented.

#### X5 [code-level, POSIX, Low] `stdc_raise` returns `true` even when the previous handler ignored the signal

`thrd_signal_handle_posix.c.ipp:364-368` — when the map has an entry for the signal
(handler installed) but no global decider claims it, `stdc_raise` calls
`invoke_sigaction(&sa, ...)` and unconditionally returns `true`. If the pre-library
handler was `SIG_IGN` (or the default action is ignore — SIGCHLD/SIGURG/SIGWINCH),
`invoke_sigaction` returns `false` (nothing happened) but `stdc_raise` still returns
`true`, violating the documented contract "returning false if we have no decider
installed for that signal" (`thrd_signal_handle.h:362-364`). The documented `false`
is only produced on the "no map entry at all" path. Callers using the documented
`if(!stdc_raise(...)) { fall back }` idiom will not detect the silently-ignored case.

#### X6 [code-level, Low] `siguninstall_system()` is a non-functional stub

`thrd_signal_handle_common.ipp.ipp:433-441` — the function only validates `version ==
0` and returns 0; it installs/removes nothing. The header documents it as "Uninstall a
previously system installed signal guard" (`thrd_signal_handle.h:423-425`), but no
system installation exists anywhere in the codebase. An API that reports success for
an operation it never performs is a latent trap for future callers (and for the
eventual C standard library integration this library targets).

#### X7 [code-level, POSIX, Low] out-of-range `signo` reaches `sigismember` without bounds checks (UB on BSD/macOS)

`thrd_signal_handle_posix.c.ipp:292` — the frame loop tests
`sigismember(frame->guarded, signo)` with no `signo < NSIG` check. On macOS/BSD,
`sigismember` is a macro expanding to `(*(set) & (1u << (signo - 1)))`
(verified via `-dM`); `stdc_raise(64, ...)` (or any signo ≥ 33 on a 32-bit sigset
platform) is a shift-count UB. On glibc, `sigismember` is a function that returns 0
for out-of-range values, so the bug is invisible there. The kernel never delivers
signo ≥ NSIG, so only direct user calls with a bogus signo reach it — but the same
call on Windows either `abort()`s (1.4/V4) or silently no-ops, i.e. three different
behaviours for the same invalid input across platforms.

#### X8 [code-level, Low] `tss_async_signal_safe_create` validates neither `val` nor `attr`

`tss_async_signal_safe.c.ipp:93-109` — `attr == NULL` → `memcpy(&mem->attr, attr, ...)`
crashes; `attr->create == NULL` is accepted and crashes later in `thread_init`; `val ==
NULL` writes through NULL. Pass 1's 2.6 covered only `destroy(NULL)`; create is
equally unguarded. Given every other public API in this library validates its
arguments (mostly via `abort()`), this one silently defers the failure.

#### X9 [code-level, Windows, Low] `asynchronous_debug_sigset` violates its documented contract

`thrd_signal_handle_windows.c.ipp:97-104` returns the empty set, while the header
documents "at least these POSIX signals are within this set: SIGQUIT, SIGTRAP,
SIGXCPU, SIGXFSZ" (`thrd_signal_handle.h:312-324`). A Windows consumer of
`sigfillset_asynchronous_debug()` gets a guard set that never matches anything — a
silent functional no-op where the documentation promises a non-empty set.

#### X10 [code-level, Medium-Low] musl (Alpine etc.) fails to compile: wrong `siginfo_t` fallback spelling

`thrd_signal_handle.h:202-208` — the platform dispatch is
`_WIN32` / `__GLIBC__` / `__ANDROID__` / `else: struct __siginfo`. musl defines
`siginfo_t` as `struct siginfo`, not `struct __siginfo`, and defines neither
`__GLIBC__` nor `__ANDROID__` — so musl builds fall into the `struct __siginfo`
branch and fail to compile. The CI matrix only exercises glibc (ubuntu-latest);
musl-based Linux (Alpine, many containers) breaks at compile time. The robust fallback
is `typedef siginfo_t` guarded by `#ifndef __GLIBC__`-style detection, or
`__has_include`-based selection.

#### X11 [code-level, Low] `sigfence` with more than 8 arguments produces a confusing hard error

`SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:85-95`) returns the 9th argument as
the count; `sigfence(a,b,c,d,e,f,g,h,i)` expands `SIGFENCE_IMPL_i` — an undefined
identifier — yielding a cryptic preprocessor/compile error rather than a diagnostic
about the 8-argument limit. (The 0-arg form `sigfence()` works; verified.)

#### X12 [test-harness, Low] `test_common.h` `thrd_join`/`thrd_create` defects

`test_common.h:48-59` — `thrd_join` checks `ret != -1`, but `pthread_join` returns an
error *number* (0 on success, e.g. `EDEADLK` on failure), never -1: on failure `*res`
is left unset while the caller proceeds as if the join succeeded. `thrd_create`
(`:43-48`) dereferences the unchecked `calloc` result (NULL deref on OOM). The
benchmark and handle tests rely on this shim; the harness masks real failures.

### 14.3 Corrections to earlier passes

- **C11 (refutes 6.1):** "`WG14_SIGNALS_PREFIX(signal_decider_destroy(sigill_decider))`
  ... with any custom prefix the tests fail to build" is **incorrect**. The macro
  argument `signal_decider_destroy(sigill_decider)` contains no prefixable identifiers
  (the argument `sigill_decider` is an unprefixed local), so the expansion
  `foo_signal_decider_destroy(sigill_decider)` is exactly the correct call. Verified:
  the library builds with `WG14_SIGNALS_PREFIX(x)=foo_##x` (42 `foo_` symbols exported)
  and all four C test programs compile (see 14.1). The spelling is still misleading
  and should be fixed for clarity, but it does not break prefix builds.
- **C12 (confirms 1.2 detail):** in the 1.2 reproduction `signal_decider_destroy`
  returns **-1** (the `ret = 0` assignment lives inside the `!is_end(it)` block, which
  is skipped for the misaligned slot) while still freeing the linked node — the
  observed return value is a second, independent symptom of the slot misalignment.
- **C13 (extends 2.1):** pass 2's 2.1 documented the *concurrent* deinit race on
  `state->count`. X1/X2 show the same `deinit_state` field is also subject to purely
  sequential use-after-frees (init/destroy after all registered threads exited) that
  require no concurrency at all — the lifetime contract of `mem->state` is simply not
  maintained when the last deinit frees it.
- **C14 (extends W2):** the W2 indeterminacy repro also demonstrated that a decider
  returning `next_decider` on a *user* raise falls through to the default action and
  the process dies (`exit 158 = SIGUSR1` default termination on macOS) — i.e. on
  POSIX an unclaimed user raise of a default-terminate signal kills the process, the
  same outcome Windows produces via WER (W5). POSIX only "returns false" when the
  signal was never installed.

### 14.4 Priority-ordered additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| X1 | High | `thread_init` UAF on `deinit_state` after all registered threads exited (verified) | `tss_async_signal_safe.c.ipp:162-166,201-214` |
| X2 | High | `destroy` UAF on `deinit_state` after all registered threads exited (verified) | `tss_async_signal_safe.c.ipp:117-121` |
| X3 | Med-High | Windows `sigguarded` never inits per-thread TSS → NULL-deref in vectored handler on fresh threads | `thrd_signal_handle_windows.c.ipp:219-249` |
| X4 | Med | Windows `EXCEPTION_STACK_OVERFLOW` unmapped (POSIX handles it as SIGSEGV) | `thrd_signal_handle_windows.c.ipp:135-164` |
| X10 | Med-Low | musl builds fail to compile (`struct __siginfo` fallback) | `thrd_signal_handle.h:207` |
| X5 | Low | `stdc_raise` returns true when the previous handler ignored the signal | `thrd_signal_handle_posix.c.ipp:364-368` |
| X6 | Low | `siguninstall_system` is a no-op stub that reports success | `thrd_signal_handle_common.ipp.ipp:433-441` |
| X7 | Low | out-of-range `signo` → `sigismember` shift UB on macOS/BSD (glibc safe) | `thrd_signal_handle_posix.c.ipp:292` |
| X8 | Low | `tss_async_signal_safe_create` validates neither argument | `tss_async_signal_safe.c.ipp:93-109` |
| X9 | Low | Windows `asynchronous_debug_sigset` returns empty set, contradicts docs | `thrd_signal_handle_windows.c.ipp:97-104` |
| X11 | Low | `sigfence` with >8 args → cryptic error | `thrd_signal_handle.h:85-95` |
| X12 | Low | `thrd_join` error check is wrong (`ret != -1`); `thrd_create` unchecked calloc | `test/test_common.h:43-59` |
| C11 | — | **Refutes 6.1**: prefix builds work; the test spelling is cosmetically wrong only | `test/*.c:101,121,111,116` |

### 14.5 Verification artifacts

New repro programs (temporary, in the review scratch dir): `repro_13.c`/`repro_setup2.c`
(1.3/1.3b), `repro_11.c` (1.1 hang), `repro_12e.c` (1.2 UAF against a library rebuilt
from a properly patched tree — the earlier header-only overlay attempt is documented
as a methodology trap), `repro_c1.c` (C1 livelock; uses SIGTRAP because
`__builtin_trap()` delivers SIGTRAP on arm64), `repro_w2.c` (W2), `repro_n66.c` (X1),
`repro_fb.c` (X2), plus `c_ho_consumer.c` (C header-only link failure),
`prefix_override.h` + `wg14-prefixed` build (C11), and a `find_package` consumer
project against a `cmake --install` prefix (V1). All compiled `-std=c11` against the
sanitized static library on macOS arm64.

---

## 15. Fifth review pass (2026-08-06, same revision): live verification, new findings, corrections

Same revision as passes 1-4 (`f48e95e`) plus the uncommitted `config.h` delta
(whitespace + `__cplusplus` in the `WG14_SIGNALS_NULLPTR` condition). Every header,
every `.ipp`, every source file, all tests, all CMake files, `verstable.h`, and the CI
matrix were re-read in full. All control-flow paths in the common core, both backends,
and `tss_async_signal_safe` were re-traced with fresh attention to (a) every `return`
statement between a `LOCK` and its matching `UNLOCK`, (b) every error path that leaves
a data structure half-committed, (c) frame-stack lifetime in `sigguarded`/`stdc_raise`
on both backends, and (d) multi-TU header-only consumption. Three new findings were
reproduced live (Y1, Y2, Y10), one probe refined an existing claim (Y9), and two
earlier claims about Windows dispatch ordering were found to be inaccurate (C15, C16).

### 15.1 Live verification results (macOS arm64, clang 17, CMake 4.3.2)

| Check | Result |
|---|---|
| Baseline rebuild (RelWithDebInfo, C11, static) + full `ctest` incl. benchmarks | 6/6 PASS |
| **NEW Y1** `install_sighandler` lock leak on `install_sighandler_impl` failure | REPRODUCED with a *library rebuilt from a forced-failure patched tree*: first `siginstall(NULL)` returns NULL; a second `siginstall(NULL)` spins at 98-99% CPU forever (killed after 3 s) — the WARNING-free `return false` at `thrd_signal_handle_common.ipp.ipp:310` never runs `UNLOCK` |
| **NEW Y2** user `longjmp` out of `guarded()` leaves `tss->front` dangling | REPRODUCED with ASan: `stack-use-after-scope` READ at `thrd_signal_handle_posix.c.ipp:292` (`sigismember(frame->guarded, signo)`) when `stdc_raise` is called after `longjmp` escaped `sigguarded` |
| **NEW Y10** multi-TU C header-only duplicate symbol | REPRODUCED: two C TUs with `WG14_SIGNALS_ENABLE_HEADER_ONLY` + forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1` → `duplicate symbol '_internal_current_thread_id_cached_set'` at link |
| **NEW Y9 probe** forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple | REPRODUCED as *silent success*: clang on macOS accepts `tls_model("initial-exec")` and the probe compiles+links — contradicting pass 1's 4.2 expectation of a compile error; the actual hazard is silent (Mach-O TLV access is not async-signal-safe) |
| Prior pass claims 1.1-1.8, V1-V8, W1-W11, X1-X12 | Unchanged by the `config.h` delta (code paths untouched); already-reproduced items not re-run |

### 15.2 New findings

#### Y1 [verified, High] `install_sighandler` leaks the global spinlock when `install_sighandler_impl` fails — a second instance of the 1.1 bug family (both backends)

`thrd_signal_handle_common.ipp.ipp:305-311`:

```c
if(!WG14_SIGNALS_PREFIX(install_sighandler_impl)(newitem, signo))
{
  int errcode = errno;
  free(newitem);
  errno = errcode;
  return false;            /* <-- no UNLOCK(state->lock) */
}
```

When the backend installation fails (POSIX `sigaction` error; Windows
`AddVectoredContinueHandler` returning NULL at `thrd_signal_handle_windows.c.ipp:417-422`),
`install_sighandler` returns `false` with `state->lock` held forever. Every subsequent
library call (`siginstall`, `siguninstall`, `signal_decider_create/destroy`,
`stdc_raise`, and any signal delivery through `raw_signal_handler`) spins forever —
the identical failure mode as 1.1, on a different error path. Verified: with
`install_sighandler_impl` forced to fail, the first `siginstall(NULL)` returns NULL and
the second call busy-spins at ~99% CPU (see 15.1, 15.5; the library itself was rebuilt
from the patched tree per the 14.1 methodology note).

Reachability differs from 1.1: on POSIX, `sigaction` cannot fail for any signal the
loop visits (SIGKILL/SIGSTOP are skipped), so the path is effectively dead there; on
Windows, `AddVectoredContinueHandler` failure is a rare resource-exhaustion event. But
the defect is real on Windows, and it converts a recoverable failure (`siginstall`
returning NULL) into a permanent, whole-library deadlock. Note also that — unlike the
other `install_sighandler` failure paths (calloc failure, TSS-create failure), which
unlock cleanly — this path returns NULL with the map untouched and `sighandlers_count`
unmodified, so the caller sees a "clean" NULL return while the library is actually
wedged (the 3.8 partial-install state is not even entered).

Fix: `UNLOCK(state->lock)` before the `return false` (same fix as 1.1).

#### Y2 [verified, POSIX, Medium] user `longjmp` out of `guarded()` leaves `tss->front` pointing at a dead frame

`thrd_signal_handle_posix.c.ipp:251-266`: `sigguarded` pushes `current` onto
`tss->front` and pops it on the two normal exits (guarded return; recovery path).
Neither the Windows backend (which never pushes) nor the docs forbid the guarded
function from using `setjmp`/`longjmp` for its own error handling; a `longjmp` out of
`guarded()` to a caller frame **above** `sigguarded` bypasses both pop sites. The
thread's `tss->front` then points into dead stack for the rest of the thread's life:

- The next `stdc_raise` on that thread walks `frame->guarded`/`frame->decider`/
  `frame->buf` in freed stack — verified ASan `stack-use-after-scope` at
  `thrd_signal_handle_posix.c.ipp:292` (see 15.1). A real signal delivered to that
  thread does the same *inside the handler*, turning a recoverable fault into a crash.
- On the async-safe TLS path the stale frame is never cleared (no re-pop anywhere
  except within `sigguarded`), so the corruption persists indefinitely.

Related sub-race (Y2b, code-level): a raise delivered after `guarded()` returns but
before `tss->front = old` executes still finds the frame, runs the frame decider, and
if it chooses `invoke_recovery` the `longjmp` makes `sigguarded` execute the recovery
path, silently discarding `guarded()`'s return value. This is a few-instruction window
in the same family as V6. Fix direction: pop the frame before calling `guarded()`
cannot work (the handler needs it), so either document the restriction or detect a
stale frame (e.g. compare the frame pointer against the current stack region on
entry to `stdc_raise`).

#### Y3 [code-level, both backends, Low] `tss_async_signal_safe_destroy` frees `mem` while still holding `mem->lock`

`tss_async_signal_safe.c.ipp:111-134`: the function `LOCK(mem->lock)` at line 116 and
never unlocks before `free(mem)` at line 132. Harmless to the object's own lifetime
(the lock word dies with the object), but:

- a user `attr.destroy` callback that re-enters the library on the *same* handle
  (e.g. `tss_async_signal_safe_get`) spins forever on the held lock;
- it is the concrete mechanism by which a concurrent thread-exit deinit (2.1)
  locks a freed object: the deinit's `LOCK(mem->lock)` at
  `tss_async_signal_safe.c.ipp:144` can observe the object after `free`.

Fix: `UNLOCK(mem->lock)` immediately before `free(mem)` (and, per 2.1, this still
does not make destroy-vs-deinit safe; it only removes the gratuitous lock state).

#### Y4 [code-level, Low] `thread_deinit`'s `attr.destroy` failure leaves a permanently stale map entry (extends 3.6)

`tss_async_signal_safe.c.ipp:150-158`: when the user's `attr.destroy` returns non-zero,
`tss_async_signal_safe_thread_deinit` returns early with the thread's TID entry still
in the map and `state->count` already decremented. The entry is never erased (nobody
else erases TIDs), so on TID reuse (3.6) a new thread observes the previous thread's
value. The count is also now inconsistent with the number of registered atexit
callbacks (the callback that failed will never re-run), so `state` may be freed while
a stale entry still points at a destroyed value. `deinit` is the only library path
that keeps the map entry on a `destroy` failure.

#### Y5 [code-level, Low] no `pthread_atfork` handling; stale TID caches are inherited across `fork()`

There are no `pthread_atfork` registrations anywhere in the library (verified by
grep). After `fork()` in a multi-threaded process, the child inherits:

- `current_thread_id_cached` (initial-exec TLS, `current_thread_id.c.ipp:50-56`) and
  `my_current_thread_id` (`tss_async_signal_safe.c.ipp:81-91`) holding the *parent's*
  TID — every `current_thread_id()`/`tss_async_signal_safe_get()` in the child returns
  the wrong identity for the child's lifetime;
- the copied `thread_id_to_tls_map` and `sig_global_state` — so the child's map
  lookups keyed by the stale parent TID can return the *parent's* per-thread values,
  and a later `thread_init` in the child inserts under the child's real TID, leaving
  two divergent entries.

For a library whose stated purpose is thread-local signal handling inside a C runtime
(the eventual integration target), fork-safety is a realistic requirement and is
neither implemented nor documented. Fix direction: `pthread_atfork` handler to reset
the cached TIDs to the tombstone (and document the child-state caveats).

#### Y6 [code-level, Low] `NSIG` is not POSIX-mandated; a missing `NSIG` silently disables `siginstall`

`thrd_signal_handle_common.ipp.ipp:58-62` uses `#if NSIG < 1024` (undefined `NSIG`
evaluates to 0 → the array variant with `arr[NSIG]` = a zero-length array, a
pedantic-mode compile error under `-Werror`), and the `siginstall`/`siguninstall`/
decider loops (`:381, :415, :454, :493, :564`) iterate `1 .. NSIG-1` — with `NSIG`
undefined the loops never execute and `siginstall` **returns success having installed
nothing**. All CI platforms define NSIG (glibc 65, macOS/BSD 32, MSVC 32), so this is
exotic-POSIX-only, but the failure is silent. Fix: fall back to a compile-time
`#ifndef NSIG` default derived from the platform, or use `_NSIG`/`NSIG`-free iteration.

#### Y7 [build, Low] the project hard-requires a C++ compiler for a C library

`CMakeLists.txt:9` declares `project(wg14_signals LANGUAGES C CXX)` and line 23 always
compiles `thread_atexit.cpp` into the library. A C-only toolchain cannot even
configure the project, and the exported static library always drags a C++ runtime
dependency into C consumers (extends 5.2). `thread_atexit` is the only C++ component
and is explicitly described as "Not mandatory and can be substituted"; making the
CXX language and/or the source conditional would restore C-only builds.

#### Y8 [code-level, Low, extends 1.8/C3] C header-only consumers would silently use per-TU global state even if linking were fixed

In C11, an inline function with a function-local `static` object gets a *separate*
object per translation unit (C11 6.7.4p3). `sig_global_state()`'s
`static struct sig_global_state_t v;` (`thrd_signal_handle_common.ipp.ipp:174-179`)
and the fallback-path static TSS slot (`:238-243`) are therefore per-TU for C
header-only consumers. (C++ is immune: inline-function statics are shared program-
wide.) Even after supplying a C `thread_atexit` (the C3 fix), a multi-TU C
header-only program would install handlers in TU A's state and raise them through TU
B's empty state — silent misbehaviour. Any fix for 1.8 must convert these to
externally-defined objects, not just add `thread_atexit`.

#### Y9 [verified probe, Low, refines 4.2] forcing `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles and links — silently unsafe

Pass 1's 4.2 predicted a compile error ("code references
`WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL` ... -> compile error") for a user-forced
`HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple. The probe shows clang on macOS *accepts*
`__attribute__((tls_model("initial-exec")))` on Mach-O (the attribute is parsed and
ignored/approximated), so the build succeeds and the real consequence is silent:
Mach-O TLS accesses (`tlv_get_addr` machinery) are not async-signal-safe, so the
library quietly violates its own "ASYNC SIGNAL SAFE" contract — including inside
`current_thread_id()`/`tss_async_signal_safe_get()` called from handlers. The failure
mode is a silent safety regression, not a diagnostic. (Same class as the 4.2 note for
non-GNU compilers, but on Apple the user is not warned at all.)

#### Y10 [verified, Low, extends 1.8/C3] multi-TU **C** header-only consumers fail with a *duplicate symbol*, not only undefined symbols

`current_thread_id.h:44-51` declares `current_thread_id_cached` (weak/selectany,
merges fine) and `internal_current_thread_id_cached_set` via `WG14_SIGNALS_EXTERN` =
`inline` in header-only mode; the definition in `current_thread_id.c.ipp:75-85` is
**not** `inline`. Per C11 6.7.4p7 the non-inline definition provides an external
symbol in *every* TU → verified `duplicate symbol '_internal_current_thread_id_cached_set'`
link error for a 2-TU C program (see 15.1). This is a distinct failure mode from C3's
undefined `thread_atexit` (which is single-TU): fixing C3 alone leaves Y10. Note the
trap: making the .ipp definition `inline` too would merely *convert* this failure into
C3's undefined-symbol failure (C11 6.7.4p7 inline definitions emit no external symbol),
so the header-only C path needs the same weak/selectany treatment the
`current_thread_id_cached` variable already gets via
`WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS` (`config.h:77-83`), or a C
`thread_atexit` plus a single external definition.

### 15.3 Corrections to earlier passes

- **C15 (refutes the dispatch-order claims in 1.6 and V5):** Windows does **not** run
  global deciders before frame deciders. `install_sighandler_impl`
  (`thrd_signal_handle_windows.c.ipp:408-427`) installs only
  `AddVectoredContinueHandler` and `SetUnhandledExceptionFilter` — there is **no**
  `AddVectoredExceptionHandler` call. Vectored *continue* handlers dispatch *after*
  frame-based EH and after the unhandled filter (the in-code comment at
  `:376-406` itself says so), and the unhandled filter runs after frame filters. So
  the effective order on Windows is: frame `__except` filters (frame deciders) →
  unhandled filter (global deciders) → continue handler (global deciders again) —
  i.e. frames-first, the same as POSIX. The genuine backend divergences are (a) the
  outcome of a *claiming* global decider: Windows `longjmp`s into `tss->front`
  (`:357-363`) — which per 1.6 is normally a *stale* `stdc_raise` frame (crash) or
  NULL (V2 crash / `EXCEPTION_CONTINUE_EXECUTION` re-fault loop), while POSIX
  executes `return true` and re-faults (C2's "both livelock" holds only for the
  no-frame case; with the 1.6 stale frame Windows crashes instead), and (b) the
  invocation-count asymmetry (C16).
- **C16 (refines V5):** the library's function runs at most **twice** per exception
  (unhandled-filter stage + continue-handler stage), and only **once** under a
  debugger (the unhandled filter is not invoked under a debugger, per the in-code
  comment), not "two or three times". The side-effecting-decider double-run claim
  stands for the no-debugger path when no decider claims.

### 15.4 Priority-ordered additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| Y1 | High | `install_sighandler` lock leak when `install_sighandler_impl` fails (verified; 1.1 family) | `thrd_signal_handle_common.ipp.ipp:305-311` |
| Y2 | Med | user `longjmp` out of `guarded()` → dangling `tss->front` → stack UAF in `stdc_raise` (verified) | `thrd_signal_handle_posix.c.ipp:251-266,292` |
| Y3 | Low | `tss_async_signal_safe_destroy` frees `mem` with lock held | `tss_async_signal_safe.c.ipp:111-134` |
| Y4 | Low | deinit `attr.destroy` failure leaves stale TID entry + count desync (extends 3.6) | `tss_async_signal_safe.c.ipp:150-158` |
| Y5 | Low | no `pthread_atfork`; stale TID caches/map across `fork()` | `current_thread_id.c.ipp:50-56,78-84`, `tss_async_signal_safe.c.ipp:81-91` |
| Y6 | Low | missing `NSIG` → zero-length array + silently no-op `siginstall` | `thrd_signal_handle_common.ipp.ipp:58-62,381` |
| Y7 | Low | unconditional CXX language + `thread_atexit.cpp` block C-only builds | `CMakeLists.txt:9,23` |
| Y8 | Low | C header-only per-TU statics in inline functions (extends 1.8/C3) | `thrd_signal_handle_common.ipp.ipp:174-179` |
| Y9 | Low | forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles silently (refines 4.2) | `config.h:41-51,53-67` |
| Y10 | Low | multi-TU C header-only: duplicate `internal_current_thread_id_cached_set` (verified; extends 1.8/C3) | `current_thread_id.c.ipp:75-85` vs `current_thread_id.h:44-51` |
| C15 | — | **Refutes 1.6/V5 ordering claim**: Windows is frames-first too; divergence is claim outcome + invocation count | `thrd_signal_handle_windows.c.ipp:408-427,376-406` |
| C16 | — | **Refines V5**: at most 2 invocations (1 under debugger), not 2-3 | `thrd_signal_handle_windows.c.ipp:376-406` |

### 15.5 Verification artifacts

New repro programs (temporary, in the review scratch dir):

- `repro_y1.c` (Y1) + a forced-failure patched copy of the tree
  (`install_sighandler_impl` returning `false`) — the *library* was rebuilt from the
  patched tree per the 14.1 methodology note. Output: `first siginstall returned
  0x0`, then the second `siginstall` busy-spins (98-99% CPU; killed after 3 s).
- `repro_y3.c` (Y2): `sigguarded` whose `guarded()` `longjmp`s out; subsequent
  `stdc_raise(SIGUSR1)` → ASan `stack-use-after-scope` READ at
  `thrd_signal_handle_posix.c.ipp:292`, compiled `-std=c11 -fsanitize=address,undefined`
  against the sanitized static library.
- `tu1.c`/`tu2.c` + `main.c` (Y10/Y9 probe): two C TUs, header-only mode,
  `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` forced on Apple → clang accepts the
  `tls_model` attribute (Y9 silent-success confirmation) and the link fails with
  `duplicate symbol '_internal_current_thread_id_cached_set'` (Y10).
- Baseline rebuild + full 6/6 ctest of the current tree (15.1).

---

## 16. Sixth review pass (2026-08-06, same revision): full re-read, live verification, new findings

Same revision as passes 1-5 (`f48e95e`) plus the uncommitted `config.h` delta. Every header,
every `.ipp`, every source file, all tests, all CMake files, the vendored `verstable.h`, the
CI matrix, and the README were re-read in full. All control-flow and error paths in the
common core, both backends, and `tss_async_signal_safe` were re-traced with fresh attention
to (a) the `#if NSIG < 1024` verstable configuration branch, (b) hand-off behaviour to
pre-existing POSIX `SA_SIGINFO` handlers, (c) every path that touches the per-thread TSS
after `siguninstall`, and (d) the documented README usage patterns versus the implementation.
Live verification was performed on macOS arm64 (clang 17, CMake 4.3.2) against the
ASan/UBSan sanitized static library rebuilt from the current tree.

### 16.1 Live verification results (macOS arm64, clang 17, CMake 4.3.2)

| Check | Result |
|---|---|
| Baseline rebuild (RelWithDebInfo, C11, static, sanitized) + full `ctest` incl. benchmarks | 6/6 PASS |
| 1.3 `sigguarded` without prior `siginstall` (fallback path) | REPRODUCED: UBSan NULL-member-access + ASan SEGV at `tss_async_signal_safe.c.ipp:177` |
| **NEW Z3** `stdc_raise(0,NULL,NULL)` after a full install→uninstall cycle (fallback path) | REPRODUCED: ASan heap-use-after-free READ at `tss_async_signal_safe.c.ipp:177` (TSS destroyed by `siguninstall`, never reset to NULL) |
| **NEW Z2** `stdc_raise(signo,NULL,NULL)` hand-off to a pre-existing `SA_SIGINFO` handler | REPRODUCED: the old handler receives `siginfo == NULL` and `ucontext == NULL`; a real `raise()` delivers a valid `siginfo` to the same handler |
| X1 `thread_init` after all registered threads exited | REPRODUCED: ASan heap-use-after-free WRITE at `tss_async_signal_safe.c.ipp:214` |
| X2 `destroy` after all registered threads exited | REPRODUCED: ASan heap-use-after-free WRITE at `tss_async_signal_safe.c.ipp:119` |
| Y2 user `longjmp` out of `guarded()` | REPRODUCED: ASan stack-use-after-scope READ at `thrd_signal_handle_posix.c.ipp:292` |
| Y10 multi-TU **C** header-only | REPRODUCED: `duplicate symbol '_internal_current_thread_id_cached_set'` |
| C3 single-TU C header-only | REPRODUCED: undefined reference to `thread_atexit` (`-Wundefined-inline`) |
| 1.8 `-DHEADER_ONLY_BUILD=ON` | REPRODUCED: `redefinition of 'get_current_thread_id'/'internal_current_thread_id_cached_set'` + `-Wstatic-in-inline` |
| V1 installed package | REPRODUCED: `cmake --install` ships no headers; `find_package(wg14_signals REQUIRED)` hard-errors on bare `check_required_components` |
| 2.8 `sigfence(<rvalue>)` | REPRODUCED: `error: invalid lvalue in asm output` |
| X11 `sigfence` with 9 args | REPRODUCED: `call to undeclared function 'SIGFENCE_IMPL_i'` |
| 0-arg `sigfence()` | compiles and runs |
| C11 custom `WG14_SIGNALS_PREFIX` | CONFIRMED: library builds with a function-like prefix (`#define WG14_SIGNALS_PREFIX(x) foo_##x`, 172 `foo_` symbols exported); note the object-like `-DWG14_SIGNALS_PREFIX=foo_` spelling does NOT work (macro is not function-like) |
| **NEW Z1** verstable `signo_to_sighandler_map_t` (the `NSIG >= 1024` branch) | REPRODUCED standalone: SIGSEGV (exit 139) on the first `_get` against a zero-initialized map — `metadata` is NULL because `signo_to_sighandler_map_t_init` is never called anywhere in the library |

### 16.2 New findings

#### Z1 [verified standalone, Medium-High] the verstable-variant `signo_to_sighandler_map_t` is never initialised — NULL `metadata` dereference on every library operation (NSIG >= 1024 platforms)

`thrd_signal_handle_common.ipp.ipp:58-135` selects the verstable hash-map variant of
`signo_to_sighandler_map_t` when `NSIG >= 1024`. Unlike `thread_id_to_tls_map_t` (which
`tss_async_signal_safe_create` initialises via `thread_id_to_tls_map_t_init`), no call to
`signo_to_sighandler_map_t_init(&state->signo_to_sighandler_map)` exists anywhere (grep
confirmed). `sig_global_state()` (`:174-179`) returns a function-local `static` struct that is
**zero-initialised**, so for the verstable variant `buckets == NULL` and `metadata == NULL`.
Verstable's `_get`/`_insert` compute `home_bucket = hash & buckets_mask` (0) and immediately
dereference `metadata[0]`:

- Reproduced standalone by instantiating the exact macro block from the library and calling
  `signo_to_sighandler_map_t_get` on a zero-initialised map: SIGSEGV (exit 139).
- Every map-touching operation (`siginstall`, `siguninstall`, `signal_decider_create/destroy`,
  `stdc_raise`'s map lookup) crashes on such a platform. Mainstream libcs (glibc 65, musl 65,
  macOS 32, FreeBSD/NetBSD 128, Solaris, AIX) never reach NSIG >= 1024, so no CI leg or test
  exercises this branch — it is a latent, configuration-specific guaranteed crash.
- Secondary defect in the same branch: the verstable variant's generated functions are named
  `signo_to_sighandler_map_t_*` (unprefixed), while the array variant's `static inline`
  functions and every call site use `WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_*)`. A
  custom-prefix build (C11) therefore fails to link on any NSIG >= 1024 platform.

Fix: call `signo_to_sighandler_map_t_init(&state->signo_to_sighandler_map)` once (or
initialise it in `sig_global_state`), and make the verstable variant prefix-aware.

#### Z2 [verified, Low-Medium] `stdc_raise(signo, NULL, NULL)` hands off to a pre-existing `SA_SIGINFO` handler with NULL `siginfo_t *` and NULL `ucontext_t *`

`thrd_signal_handle_posix.c.ipp:314-368`: when no frame or global decider claims the raise and
the pre-library handler was installed with `SA_SIGINFO`, `invoke_sigaction` calls
`sa->sa_sigaction(signo, NULL, NULL)` — the `info`/`raw_context` arguments of a user
`stdc_raise(signo, NULL, NULL)` are passed through unchanged. Verified: with an application
`SA_SIGINFO` handler captured as `old_handler`, `stdc_raise(SIGUSR1, NULL, NULL)` invokes the
handler with `si == 0x0` (a real `raise()` delivers a valid `si`). Any application handler that
reads `si->si_signo`/`si->si_addr` (the normal pattern for SA_SIGINFO handlers) crashes. The
library's own `raw_signal_handler` always passes the kernel's real `siginfo`, so this is
specific to the documented "pass on signal handling to this library" API (`stdc_raise` with
NULL info). The header does not warn that the previous handler may receive NULL pointers.

Fix direction: either synthesise a minimal `siginfo_t` when `info == NULL` (as the
documentation implies the caller may do), or document the NULL-pointer hand-off explicitly.

#### Z3 [verified, High, fallback-TLS platforms] every `sigguarded`/`stdc_raise` after a full `siguninstall` is a heap-use-after-free on the destroyed TSS handle (extends 2.4 / V7)

`thrd_signal_handle_common.ipp.ipp:276-281`: `sig_global_tss_state_destroy`'s intended reset of
`*sig_tss_state_raw()` to NULL is dead code (unreachable after `return`). After the final
`siguninstall` on the fallback path, `tss_async_signal_safe_destroy` frees the shared
`tss_async_signal_safe` object but the static slot still points at it. Verified with ASan:

- `stdc_raise(0, NULL, NULL)` (the documented per-thread setup call) after install→uninstall:
  heap-use-after-free READ at `tss_async_signal_safe.c.ipp:177` (the `LOCK(mem->lock)` at
  `thread_init` entry).
- `sigguarded(...)` likewise.
- Any real signal delivered to a thread that re-enters the library after a second `siginstall`
  also passes through `sig_global_tss_state_init` and hits the dangling handle; only a fresh
  `siginstall` (which overwrites the slot via `sig_global_tss_state_create`) restores sanity.

Passes 1-2 framed this as dead code (2.4) and as a concurrent-uninstall hazard (V7); this
shows the purely sequential, documented-usage failure: install once, uninstall once, then use
any `sigguarded`/`stdc_raise` → UAF. Fix: reset `*sig_tss_state_raw()` to NULL on destroy
(i.e. actually execute the dead line), and additionally guard `thread_init` against a NULL
handle (see 1.3).

#### Z4 [code-level, Low, extends X7/V4] negative `signo` in `stdc_raise` is UB on the POSIX frame walk and `abort()`s on Windows

`thrd_signal_handle_posix.c.ipp:292`: `sigismember(frame->guarded, signo)` with a negative
`signo` expands (macOS/BSD macro form) to `1u << (signo - 1)` — a negative/oversized shift
count, i.e. UB, whenever any frame exists on the thread. The map lookup is bounds-checked and
returns "not installed", but only after the frame walk. On Windows, `stdc_raise(-1, ...)` →
`win32_exception_code_from_signal` `default: abort()`. So the three platforms give three
different behaviours for the same invalid input (UB, `false`, `abort`), and X7's noted
positive out-of-range case has a negative sibling.

#### Z5 [code-level, Windows, Low] `siguninstall` clobbers an application-installed `SetUnhandledExceptionFilter`

`thrd_signal_handle_windows.c.ipp:423-424,437-438`: the library captures the unhandled-exception
filter present at first `siginstall` and restores exactly that filter on full uninstall. If the
application installs its own filter *after* the library's `siginstall`, the library's
`siguninstall` overwrites the application's filter with the stale pre-library one. The
unhandled-exception filter is a single process-global slot, and ownership transfer on
uninstall is asymmetric with the app's expectations (the app never gets a chance to restore
its filter). Minor, but a silent application-behaviour change on teardown.

#### Z6 [code-level, Linux/glibc, Low] `siginstall(NULL)` installs handlers for glibc-internal signals 32/33 and realtime signals 34-64

On glibc `NSIG == 65`, so `siginstall(NULL)` (the pattern used by every test and the README)
installs the library's `SA_NODEFER|SA_NOCLDWAIT|SA_SIGINFO` handler for signals 32
(`SIGCANCEL`) and 33 (`SIGSETXID`, glibc's internal pthread-cancellation/setxid signals) and
for all 31 realtime signals 34-64:

- `SIGCANCEL`/`SIGSETXID`: the library *replaces* glibc's internal handler and chains to it via
  `invoke_sigaction` on each delivery, adding latency to every pthread cancellation and —
  because global deciders run first — allowing a decider to swallow cancellation. Also,
  `SA_NODEFER` on the internal signals changes glibc's expected blocking semantics.
- Realtime signals: the default path in `invoke_sigaction` resets to `SIG_DFL` and re-raises,
  so a realtime-signal delivery (e.g. a POSIX timer via `timer_create(..., SIGEV_SIGNAL)`) is
  routed through the library and its default re-raise discards the library handler
  permanently (the 3.4/C10 family), even though `sigfillset_synchronous`/`_asynchronous_*`
  deliberately do not include realtime signals. The `SIGKILL`/`SIGSTOP` skip is correct, but
  the internal and realtime ranges are neither skipped nor documented.

#### Z7 [code-level, Low] `sigfillset_synchronous` / `_asynchronous_nondebug` / `_asynchronous_debug` crash on a NULL `set`

All three implementations do `memcpy(set, ..., sizeof(*set))` (POSIX) or the equivalent
(Windows) with no NULL check — a NULL argument is a NULL deref. Every other argument-taking
API in the library either validates (mostly via `abort()`) or returns an error; these three
silently crash.

#### Z8 [code-level, Low, extends X8/2.6] `tss_async_signal_safe_destroy` double-destroy and NULL-handle use are unguarded

`tss_async_signal_safe_destroy(NULL)` dereferences NULL at `LOCK(mem->lock)` (2.6); a second
`destroy` on the same handle locks freed memory. `tss_async_signal_safe_get`/`thread_init` with
a NULL or already-destroyed handle crash identically. Pass 1's 2.6 and pass 4's X8 covered
`create`/`destroy(NULL)` separately; the *double-destroy* and the post-destroy use of `get`/
`thread_init` are the same family and equally undocumented.

#### Z9 [code-level, Low] `thread_init`'s unlocked `attr.create` breaks the THREADSAFE contract for concurrent first-use on distinct threads

`tss_async_signal_safe.c.ipp:184-191`: the user's `create` callback runs **without**
`mem->lock`, so two threads racing to first-initialise the same handle call the user's `create`
concurrently. The API documents `thread_init` as THREADSAFE, and the library serialises the map
insert but not the create callback. The test suite's own `create` uses a shared
`static unsigned *storage_ptr` (`async_signal_safe_tls_test.c:7`), so a two-worker concurrent
first-use would be a data race in the harness itself (and the 2-element `storage` makes a third
init an overflow — 6.2/C7). Design note: either document that `create` must be thread-safe, or
serialise the create callback under the lock (at the cost of re-entrancy, cf. 3.13).

#### Z10 [code-level, Windows, Low, extends X9] `asynchronous_nondebug_sigset` silently omits most documented signals and includes the two `siginstall`-skipped ones

`thrd_signal_handle_windows.c.ipp:73-89` builds the nondebug set from only
`{SIGINT, SIGKILL, SIGSTOP, SIGTERM}`, whereas the header documents it as containing at least
SIGALRM, SIGCHLD, SIGCONT, SIGHUP, SIGINT, SIGKILL, SIGSTOP, SIGTERM, SIGTSTP, SIGTTIN,
SIGTTOU, SIGUSR1, SIGUSR2, SIGPOLL, SIGPROF, SIGURG, SIGVTALRM. Additionally `SIGKILL` and
`SIGSTOP` (which `siginstall` deliberately skips) are in the set, so
`siginstall(sigfillset_asynchronous_nondebug())` on Windows claims two signals that are never
installed. This is the nondebug-set sibling of X9 (the empty debug set).

#### Z11 [documentation, Low-Medium] the README's standalone `sigguarded` example is non-functional on every platform

`Readme.md:20-33` shows `sigguarded(...)` with no preceding `siginstall`. On POSIX the frame
stack is only consulted by the library's own `raw_signal_handler` (installed only by
`siginstall`) or by user `stdc_raise`; a genuine fault for an uninstalled signal runs the
kernel default (terminate) and never reaches the guard. On fallback-TLS platforms the example
crashes outright (1.3, verified) because `sig_global_tss_state_init` NULL-derefs. On Windows,
`sigguarded` alone leaves the per-thread TSS uninitialised (X3). The header's own docs
(`thrd_signal_handle.h:341-346`) recommend `stdc_raise(0, nullptr, nullptr)` as the setup
step — which itself crashes on the fallback path (1.3b) and aborts on Windows (1.4). The
README example should call `siginstall` (and the setup call must be fixed first).

### 16.3 Corrections and extensions to earlier passes

- **C17 (extends 1.8/12.1/14.4):** a plain `-DWG14_SIGNALS_PREFIX=foo_` on the compiler command
  line does **not** work (the macro becomes object-like; `WG14_SIGNALS_PREFIX(thread_id_t)`
  then token-pastes incorrectly, producing a cascade of "parameter list without types" errors).
  The C11 claim (prefix builds work) holds only for a **function-like** definition such as
  `#define WG14_SIGNALS_PREFIX(x) foo_##x`, which this pass re-confirmed (172 `foo_` symbols).
  This is a documentation gap in `config.h` (the prefix macro is never demonstrated), not a new
  library defect.
- **C18 (confirms and scopes 2.4/V7):** the post-uninstall UAF (Z3) is purely sequential and
  reachable through the documented setup call on fallback platforms; V7's concurrent framing
  and 2.4's dead-code framing are two sides of the same missing reset. The sequential form was
  previously unverified — this pass reproduces it.
- **C19 (notes on the pass-5 dispatch debate, C15/C16):** the Windows invocation-count question
  (does the library function run once or twice per exception when a decider claims and returns
  `EXCEPTION_CONTINUE_EXECUTION`?) could not be re-resolved on this host. However, the Windows
  CI `thrd_signal_handle_test` asserts `count_decider == 1` for a global decider invoked via
  `stdc_raise`, and that CI leg passes — which is consistent with the library function running
  exactly once on the no-debugger path (unhandled-filter stage) and once on the debugger path
  (continue-handler stage), i.e. C16's "at most twice" is itself probably too pessimistic for
  the no-debugger case. Marked as unresolved rather than a new claim.
- **C20 (extends 3.4/C10):** the `invoke_sigaction` SIG_DFL re-raise path also applies to
  glibc realtime signals 34-64 installed by `siginstall(NULL)` (Z6), i.e. the "reset to
  SIG_DFL and re-raise" behaviour silently discards the library's handler for an entire class
  of signals the documented `sigfillset_*` sets deliberately exclude.

### 16.4 Priority-ordered additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| Z1 | Med-High | verstable `signo_to_sighandler_map_t` never initialised → NULL-metadata crash (NSIG >= 1024); also not prefix-aware | `thrd_signal_handle_common.ipp.ipp:58-135,174-179` |
| Z3 | High | `sigguarded`/`stdc_raise` after full `siguninstall` = heap-UAF on destroyed TSS (verified; extends 2.4/V7) | `thrd_signal_handle_common.ipp.ipp:276-281`, `tss_async_signal_safe.c.ipp:177` |
| Z2 | Low-Med | `stdc_raise(signo,NULL,NULL)` hands NULL `siginfo_t*`/`ucontext_t*` to pre-existing SA_SIGINFO handler (verified) | `thrd_signal_handle_posix.c.ipp:314-368` |
| Z6 | Low | `siginstall(NULL)` on glibc installs over `SIGCANCEL`/`SIGSETXID` (32/33) and realtime 34-64; default re-raise discards handler | `thrd_signal_handle_posix.c.ipp:371-383,143-184`, `thrd_signal_handle_common.ipp.ipp:381-405` |
| Z4 | Low | negative `signo` → UB frame-walk shift / Windows `abort()` (extends X7/V4) | `thrd_signal_handle_posix.c.ipp:292`, `thrd_signal_handle_windows.c.ipp:112-134` |
| Z5 | Low | Windows `siguninstall` clobbers an app filter installed after `siginstall` | `thrd_signal_handle_windows.c.ipp:437-438` |
| Z7 | Low | `sigfillset_*` NULL `set` → `memcpy` NULL-deref | both backends |
| Z8 | Low | `tss_async_signal_safe` NULL/double destroy + post-destroy `get`/`thread_init` unguarded | `tss_async_signal_safe.c.ipp:111-134,226-243` |
| Z9 | Low | `thread_init`'s unlocked `attr.create` breaks THREADSAFE claim for concurrent first-use | `tss_async_signal_safe.c.ipp:184-191` |
| Z10 | Low | Windows nondebug set omits documented signals, includes `SIGKILL`/`SIGSTOP` (extends X9) | `thrd_signal_handle_windows.c.ipp:73-89` |
| Z11 | Doc | README standalone `sigguarded` example non-functional on all platforms | `Readme.md:20-33` |

### 16.5 Verification artifacts

Temporary repro programs (in the review scratch dir): `z1_repro.c` (Z1 standalone verstable
crash), `repro13.c` (1.3), `n1_repro.c` (Z2 NULL-siginfo hand-off), `uninst_repro.c` (Z3
post-uninstall UAF), `x1_repro.c`/`x2_repro.c` (X1/X2), `y2_repro.c` (Y2),
`tu1.c`/`tu2.c`/`tuh.c` (Y10), `ho_single.c` (C3), `ho_build` dir (`-DHEADER_ONLY_BUILD=ON`),
`install_prefix`/`consumer` (V1), `fence_rv.c`/`fence_9.c`/`fence_0.c` (2.8/X11),
`prefix_override.h` + `prefixed` build (C11). All C repros compiled `-std=c11
-fsanitize=address,undefined` against the sanitized static library on macOS arm64.

---

## 17. Seventh review pass (2026-08-06, same revision): fresh re-read, live verification, new findings

Same revision as passes 1-6 (`f48e95e`) plus the uncommitted `config.h` delta. Every header,
every `.ipp`, every source file, all tests, all CMake files, `verstable.h`, the CI matrix,
the toolchain files, and the README were re-read in full. All control-flow and error paths in
the common core, both backends, and `tss_async_signal_safe` were re-traced again, this time
with fresh attention to (a) sequential (non-concurrent) API sequences that pass through
`siguninstall` while deciders or frames are still live, (b) the build-configuration matrix
beyond what CI exercises (the filc toolchain, `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`, forced
feature flags), and (c) the refcount protocol of `global_signal_decider_t` (confirmed sound
after a fresh trace — see C21). Live verification was performed on macOS arm64 (clang 17,
CMake 4.3.2) against the ASan/UBSan sanitized static library rebuilt from the current tree.

### 17.1 Live verification results (macOS arm64, clang 17, CMake 4.3.2)

| Check | Result |
|---|---|
| Baseline rebuild (RelWithDebInfo, C11, static, sanitized) + `ctest -E benchmark` | 4/4 PASS |
| **NEW AA1** decider orphan crash (`siguninstall` → `siginstall` → `signal_decider_destroy`) | REPRODUCED: UBSan null-member-access + ASan SEGV (WRITE to address 0x0) at `thrd_signal_handle_common.ipp.ipp:590` (`LIST_REMOVE` expansion inside `signal_decider_destroy`), exit 134 |
| **NEW AA3** build of `thrd_sigfpe_test.c` with `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` defined | REPRODUCED: compile error at `test/thrd_sigfpe_test.c:64` — `sigfence` undeclared (the test calls it unconditionally) |
| **NEW AA2** library compile with `-DDISABLE_INLINE_ASM=1` | Compiles unchanged and still emits the GNU inline-asm `SIGFENCE_IMPL_*` path — the macro is honoured only by `test/ticks_clock.h`, never by the library |
| Prior pass claims 1.1-1.8, V1-V8, W1-W11, X1-X12, Y1-Y10, Z1-Z11 | Unchanged by the `config.h` delta (code paths untouched); already-reproduced items not re-run |

### 17.2 New findings

#### AA1 [verified, Medium-High] `signal_decider_destroy` crashes after a `siguninstall` → `siginstall` cycle orphans the decider node (sequential, no concurrency)

`thrd_signal_handle_common.ipp.ipp:443-614`. `siguninstall` (via `uninstall_sighandler`,
`:328-364`) frees the per-signal `sighandler_info` container as soon as its refcount hits
zero — but it does **not** look at the container's `global_handler` list. Any decider node
still linked there survives the container free with its `prev`/`next` pointers pointing into
freed memory (or NULL). A later `signal_decider_destroy(handle)` for that decider then:

1. finds the *reinstalled* signal's map entry (new container), decrements the node's
   refcount to 0, and executes
   `LIST_REMOVE(value(it)->global_handler, *retp)` at `:590` against the **new** (empty)
   list, using the orphaned node's stale `next`/`prev` — for a single-node list
   (`callfirst=false`, `LIST_INSERT_BACK` on an empty list leaves `item->next =
   item->prev = NULL`), `LIST_REMOVE` falls to its final branch `(item)->next->prev =
   (item)->prev` → **NULL-pointer write** (verified: ASan SEGV, WRITE to 0x0, at `:590`).
   With multiple orphaned nodes the neighbour pointers point into the *freed container* →
   heap-use-after-free instead.

The exact sequential sequence that crashes (all public, documented API):

```c
h  = siginstall(&g);                  // g = {SIGUSR1}
d  = signal_decider_create(&g, ...);  // node linked into SIGUSR1's global_handler
siguninstall(h);                      // frees the container; node orphaned
h2 = siginstall(&g);                  // new container for SIGUSR1
signal_decider_destroy(d);            // LIST_REMOVE on orphaned node -> crash
```

Without the reinstall, `signal_decider_destroy` still frees the orphaned node cleanly (the
map lookup returns end, the in-lock block is skipped, and the slot is freed out-of-lock), so
the crash specifically requires the reinstall. This is the purely sequential sibling of the
concurrent container UAF (2.2/W4): same root cause (container freed without regard for live
decider nodes), distinct trigger requiring no threads at all. It also means `siguninstall`
*silently orphans* any decider still registered for the uninstalled signal — the decider is
gone from the reinstalled signal's chain, which is itself undocumented behaviour.

Fix direction: on container teardown, either unlink-and-defers-free any nodes still in
`global_handler` (and have `signal_decider_destroy` tolerate nodes already removed), or have
`signal_decider_destroy` validate that `*retp` is still linked into `value(it)->global_handler`
before `LIST_REMOVE`.

#### AA2 [code-level, build-config, Low] the filc toolchain's `-DDISABLE_INLINE_ASM=1` is a no-op for the library; the toolchain also hardcodes machine-specific paths

`cmake/filc-toolchain.cmake:3-4` passes `-DDISABLE_INLINE_ASM=1` for both C and C++.
`DISABLE_INLINE_ASM` is referenced **only** by `test/ticks_clock.h:44` (the benchmark
clock); the library itself never tests it. The library's `SIGFENCE_IMPL_*` selection in
`thrd_signal_handle.h:97-154` keys off `__GNUC__ || __clang__` only — filc is clang-based, so
`__clang__` is defined and the GNU inline-asm path is compiled regardless of the flag. The
toolchain's apparent intent (disable inline asm under filc) is not achieved; if filc's
assembler rejects the `__asm__ volatile(";")` forms, the library build fails. Additionally
the toolchain hardcodes `/home/ned/Downloads/filc-0.668.2-...` compiler paths (not portable),
and this toolchain is never exercised by CI.

#### AA3 [verified, build-config, Low] `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` breaks the test suite

`thrd_signal_handle.h:77` gates the whole `sigfence` macro (and the
`sigfence_force_escaped` declaration) behind `#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`,
but `test/thrd_sigfpe_test.c:64` calls `sigfence(result)` unconditionally. Defining the
macro (a legitimate, if undocumented, config knob) makes the test fail to compile
(verified: `call to undeclared function 'sigfence'`). There is no CI configuration that
tests the macro, and `config.h` does not document it.

#### AA4 [code-level, POSIX, Low] `siguninstall` silently discards application handler changes made during the library's tenure

`thrd_signal_handle_posix.c.ipp:385-390` (`uninstall_sighandler_impl`) restores
`item->old_handler` — the handler captured at `siginstall` time — via `sigaction(signo,
&item->old_handler, NULL)`. Any `sigaction()`/`signal()` call the application makes *after*
`siginstall` is therefore overwritten at `siguninstall`, reverting the slot to the
pre-library handler even though the application deliberately changed it mid-tenure. This is
the POSIX sibling of the Windows `SetUnhandledExceptionFilter` clobber (Z5); the header's
warning ("NOT threadsafe with respect to other code modifying the global signal handlers",
`thrd_signal_handle.h:414-416`) is framed as a concurrency caveat and does not cover the
sequential case.

#### AA5 [code-level, both backends, Low] a *global* decider returning `sig_decision_invoke_recovery` has divergent, undocumented semantics

The enum documentation (`thrd_signal_handle.h:254-257`) says `sig_decision_invoke_recovery`
is "Thread local signal deciders only", yet global deciders share the same `sig_decide_t`
type and both backends accept it:

- **POSIX** (`thrd_signal_handle_posix.c.ipp:357-361`): `if(res)` treats *any* non-zero
  decision as "claim and `return true`" — for `invoke_recovery` the raise is claimed, no
  recovery is ever called, and for a genuine fault the handler returns and the faulting
  instruction re-executes (the C1/C2 livelock), **even when a guarding `sigguarded` frame
  exists** (the frame walk ran first, returned `next_decider`, and there is no path that
  turns a global decider's `invoke_recovery` into a frame recovery).
- **Windows** (`thrd_signal_handle_windows.c.ipp:354-366`): the same value causes a
  `longjmp(tss->front->buf, 1)` into the top frame when one exists (or
  `EXCEPTION_CONTINUE_EXECUTION` / NULL-deref otherwise) — i.e. it behaves like
  "resume at the top frame".

So one enum value produces "claim, no recovery, re-fault" on POSIX and "unwind to top
frame" on Windows. Neither backend documents or diagnoses this for global deciders.

#### AA6 [code-level, Windows, Low] user `EXCEPTION_RECORD` parameters masquerade as `rsi->addr` / `rsi->error_code`

`thrd_signal_handle_windows.c.ipp:186-191` reads `ExceptionInformation[1]` and
`ExceptionInformation[2]` as `addr` and `error_code` with no `NumberParameters` check. For a
user raise via `stdc_raise(signo, info, ctx)` the `ExceptionInformation[]` array holds the
*caller's own parameters* (whatever was in `info`), so deciders see arbitrary user data in
`addr`/`error_code` — and for a zero-parameter raise (`stdc_raise(signo, NULL, NULL)`,
`RaiseException(code, 0, 0, NULL)`) the reads are past the valid prefix (the 1.5 family, but
with the additional observation that for non-NULL `info` the two fields are *deterministic
user data*, not garbage). Any decider keying on the NTSTATUS in `error_code` gets different
values for user raises than for genuine faults.

#### AA7 [code-level, C++ conformance, Low] `calloc` allocates C++ objects containing `std::atomic_uint` members without starting their lifetime

`tss_async_signal_safe.c.ipp:93-109` (`tss_async_signal_safe_create`) and `:203-204`
(`deinit_state` allocation) use `calloc` for structs whose members include
`std::atomic_uint` (the `lock` and `count` fields). In C++ — the library is documented and
tested as C++-usable (header-only C++ test), and `thread_atexit` is compiled as C++ — no
constructor runs for those atomics, so using them is object-lifetime UB per the C++ standard
(works on MSVC/GCC/Clang because `std::atomic<unsigned>` is trivially default-constructible
in practice). The same pattern is latent for any future non-trivially-constructible member.
The C path is unaffected (C11 `atomic_uint` is a plain type).

#### AA8 [code-level, Low] the `tss_async_signal_safe` per-thread ID cache uses plain `_Thread_local`, not the async-signal-safe attribute

`tss_async_signal_safe.c.ipp:81-91` declares the `my_current_thread_id()` cache with
`WG14_SIGNALS_THREAD_LOCAL` (plain `_Thread_local`), not
`WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL`. On platforms where the library already has
async-signal-safe TLS available (Linux/Windows, the async-safe path), this cache is still
global-dynamic TLS on ELF, whose first access calls `__tls_get_addr` — not
async-signal-safe. The cache is normally primed by the documented `thread_init` call, so
`tss_async_signal_safe_get` (documented ASYNC-SIGNAL-SAFE) is only actually safe after
priming; the first `get` from a handler on a never-primed thread is async-signal-unsafe even
on Linux. Extends the 4.3/Y9 family to the async-safe path's own cache.

#### AA9 [code-level, Windows, Low] `stdc_raise(SIGFPE)` raises a different exception code than a genuine integer divide-by-zero

`win32_exception_code_from_signal` maps SIGFPE → `EXCEPTION_FLT_INVALID_OPERATION`
(0xC0000090), but the real hardware fault from `x / 0` on x64 is
`EXCEPTION_INT_DIVIDE_BY_ZERO` (0xC0000094); both reverse-map to SIGFPE. A decider that
inspects `rsi->error_code` (documented as "the NTSTATUS code") therefore observes different
codes for the same logical signal depending on whether it was user-raised or a real fault.
Similarly `stdc_raise(SIGBUS)` maps to `EXCEPTION_IN_PAGE_ERROR`, a semantically different
fault class from a real SIGBUS-equivalent (`EXCEPTION_ACCESS_VIOLATION` on a guard page is
more common). Cosmetic divergence, but the README/header invite reading `error_code`.

### 17.3 Corrections and extensions to earlier passes

- **C21 (confirms 3.10 / the refcount protocol):** a fresh trace of the raise/destroy
  interleaving confirms the `global_signal_decider_t` refcount protocol is sound: the base
  refcount is 1 (create), an in-flight raise increments to 2 *before* the unlocked decider
  call, and `signal_decider_destroy`'s `--refcount` then lands on 1 and takes the
  deferral branch (`*retp = NULL`, "freed when the handler exits"). The raise's later
  `--refcount` reaches 0 and moves the node to `deferred_frees`. The concurrent-destroy test
  genuinely exercises this deferred path and cannot free a node mid-decider. No change to
  earlier claims; recorded because the raise-side arithmetic (`--` after `++`) is easy to
  mis-read as broken (this pass initially did).
- **C22 (scopes AA1 vs 2.2/W4):** the container-orphan crash (AA1) is the *sequential*
  form of the same defect family as the *concurrent* container UAF (2.2/W4). Pass 1's 2.2
  and pass 3's W4 require a concurrent `siguninstall` racing an in-flight raise; AA1 requires
  no concurrency at all and is deterministic through documented API calls. The common root
  cause — `uninstall_sighandler` freeing `sighandler_info` without accounting for nodes still
  linked in its `global_handler` list — should be fixed once for all three.
- **C23 (notes on the filc toolchain):** the previous passes reviewed `filc-toolchain.cmake`
  only implicitly (as an unexercised build option); this pass confirms the `DISABLE_INLINE_ASM`
  flag it relies on is not referenced by any library code (grep: only `test/ticks_clock.h`),
  so the toolchain's intent is unimplemented in the library (AA2).

### 17.4 Priority-ordered additions

| # | Severity | Issue | Location |
|---|----------|-------|----------|
| AA1 | Med-High | `signal_decider_destroy` NULL-deref/UAF after `siguninstall`→`siginstall` orphans the decider node (verified) | `thrd_signal_handle_common.ipp.ipp:328-364,443-614` |
| AA2 | Low | filc `-DDISABLE_INLINE_ASM=1` is a no-op for the library (GNU inline asm still emitted); toolchain paths hardcoded | `thrd_signal_handle.h:97-154`, `cmake/filc-toolchain.cmake` |
| AA3 | Low | `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` breaks the test build (verified) | `thrd_signal_handle.h:77`, `test/thrd_sigfpe_test.c:64` |
| AA4 | Low | `siguninstall` (POSIX) discards post-`siginstall` app handler changes (Z5 sibling) | `thrd_signal_handle_posix.c.ipp:385-390` |
| AA5 | Low | global decider `invoke_recovery`: POSIX claims-without-recovery vs Windows unwinds-to-frame (undocumented divergence) | `thrd_signal_handle_posix.c.ipp:357-361`, `thrd_signal_handle_windows.c.ipp:354-366` |
| AA6 | Low | Windows user `EXCEPTION_RECORD` params leak into `rsi->addr`/`error_code` | `thrd_signal_handle_windows.c.ipp:186-191` |
| AA7 | Low | C++ object-lifetime UB: `calloc` for `std::atomic_uint` members | `tss_async_signal_safe.c.ipp:93-109,203-204` |
| AA8 | Low | `my_current_thread_id` cache uses plain `_Thread_local`, not async-safe TLS | `tss_async_signal_safe.c.ipp:81-91` |
| AA9 | Low | `stdc_raise(SIGFPE)` code ≠ real `INT_DIVIDE_BY_ZERO` code; `SIGBUS`→`IN_PAGE_ERROR` | `thrd_signal_handle_windows.c.ipp:112-134` |
| C21 | — | **Confirms 3.10**: decider refcount protocol is sound; concurrent-destroy test exercises the deferral path | `thrd_signal_handle_posix.c.ipp:334-362` |

### 17.5 Verification artifacts

- `repro_orphan.c` (AA1): compiled `-std=c11 -fsanitize=address,undefined` against the
  sanitized static library on macOS arm64; output shows the crash (UBSan null-member-access
  then ASan SEGV, WRITE to 0x0, at `thrd_signal_handle_common.ipp.ipp:590`, exit 134).
- AA3: `cc -DWG14_SIGNALS_DISABLE_SIGFENCE_MACRO -fsyntax-only test/thrd_sigfpe_test.c` →
  `call to undeclared function 'sigfence'` at line 64.
- AA2: `cc -DDISABLE_INLINE_ASM=1 -fsyntax-only src/wg14_signals/thrd_signal_handle_posix.c`
  succeeds (flag ignored by the library); grep confirms `DISABLE_INLINE_ASM` appears only in
  `test/ticks_clock.h` and the toolchain.
- Baseline rebuild + `ctest -E benchmark` of the current tree: 4/4 pass (17.1).
