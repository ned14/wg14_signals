# Exhaustive implementation analysis: wg14_signals

Review dates: 2026-08-05 and 2026-08-06 (seven full review passes, all against the same
revision). Revision reviewed: `f48e95e` ("Implement all the changes as per N3924 WIP
wording for 'Thread-safe signals handling rev 4'"), plus one uncommitted
whitespace/`nullptr`-for-C++ change in `config.h`.

Scope: every header, every source file, both backends (POSIX/Windows), the header-only
configuration, the fallback hash-table TLS path and the async-signal-safe TLS path, all
error paths, and all build configurations exercised and *not* exercised by CI.

Findings are ranked by severity. Items marked **[confirmed]** were reproduced on macOS
(arm64, clang 17, ASan/UBSan where noted). Windows-only items are code-level findings (no
Windows host available) but were verified against the MSVC build matrix in CI. Finding
IDs are stable and are referenced by `plans/ideas.md`. Later-pass findings keep their
pass letter (V = pass 2, W = pass 3, X = pass 4, Y = pass 5, Z = pass 6, AA = pass 7);
corrections from later passes are folded into the finding they affect.

---

## 2. High-severity issues (static analysis and verified)

### 2.1 `tss_async_signal_safe` deinit race -> use-after-free of the `deinit_state` **[FIXED 2026-08-10]**

`tss_async_signal_safe.c.ipp:136-168`. The thread-exit callback
`tss_async_signal_safe_thread_deinit` decrements `state->count` and frees `state`
*outside* `mem->lock` (after `UNLOCK` at line 160). Two threads sharing the same `tss`
that exit at roughly the same time can interleave: thread A erases its own entry and
`UNLOCK`s; thread B does the same, its `fetch_sub` returns 1 and it `free(state)`s; thread
A then executes `atomic_fetch_sub(&state->count, ...)` on freed memory (UAF), or reads
`state->val` concurrently with `tss_async_signal_safe_destroy`'s `state->val = NULL`
write. **Correction (C13):** the same `deinit_state` field is also subject to purely
*sequential* UAFs (X1/X2, below) — the lifetime contract of `mem->state` is simply not
maintained when the last deinit frees it. Additionally, `tss_async_signal_safe_destroy`
frees `mem` while other threads' atexit handlers may still hold `state->val == mem`. The
intended usage (destroy only after all threads joined) is not documented.

**Fixed in `tss_async_signal_safe.c.ipp` (deinit rewrite, `:139-180`):** the count
decrement and the `free(state)` now both execute *under* `mem->lock`, so concurrent
thread-exit deinits and `destroy` serialise and no deinit can free `state` while another
thread is still decrementing it or reading `state->val`; the last deinit clears
`mem->state` (guarded by `state == mem->state`) before freeing, fixing X1/X2 as well. The
destroy-after-join requirement is now documented in `tss_async_signal_safe.h`; the
`attr.destroy` callback value is captured under the lock before the unlocked callback
call, and a non-zero callback return no longer leaks the TID entry or the count (Y4).
**Follow-up fix (Linux LSan CI):** moving the count decrement + `free(state)` inside the
`mem != NULL` block made a thread whose instance was destroyed while it was still
registered (the tests do exactly this) skip both, leaking the 16-byte `deinit_state`.
The `mem == NULL` branch now still drops the thread's reference (decrement `state->count`,
free on zero) — `destroy` clears `state->val` but never frees `state`; the last
still-registered thread's deinit does. Verified against the exact CI failure: under Linux
ASan/LSan (arm64 Ubuntu container) `async_signal_safe_tls_test` and `header_only_test`
reproduce the reported 16-byte leak on the unfixed code and are clean with the fix.
**Verified:** macOS arm64, clang 22, ASan/UBSan. The new regression test
`test/tss_concurrent_exit_test.c` (registered as `tss_concurrent_exit_test` in
`test/CMakeLists.txt`) reproduces the ASan `heap-use-after-free` WRITE at
`tss_async_signal_safe.c.ipp:217` on the unfixed library and now passes, as does its
1000-iteration two-thread same-tss concurrent-exit stress; the full `ctest` suite (13
tests) passes.

### 2.2 `siguninstall` / `signal_decider_destroy` vs. in-flight `stdc_raise` -> use-after-free of `sighandler_info` **[FIXED 2026-08-10]**

In `stdc_raise` (POSIX, `thrd_signal_handle_posix.c.ipp:314-368`) the `state->lock` is
released around each `current->decider(&rsi)` call. If another thread runs `siguninstall`
during that window and the per-signal refcount drops to zero, it frees the
`sighandler_info` container (the map entry holding `global_handler`/`deferred_frees`
list heads) while `stdc_raise` still holds the `it` pointer and re-accesses
`value(it)->global_handler` after re-acquiring the lock -> UAF. **Correction (C5):** the
decider-node refcount protocol itself is sound (a raise's `refcount++` always precedes
the unlocked decider call); the UAF is strictly the container. **Correction (C6/W4):**
the identical pattern exists in the Windows vectored handler
(`thrd_signal_handle_windows.c.ipp:329-368`), which additionally may be *removed*
(`RemoveVectoredContinueHandler`) mid-raise when the count reaches zero. The header
documents siginstall as threadsafe only with respect to other concurrent executions of
itself, but nothing prevents the user from doing this.

**Fixed:** `sighandler_info` now carries a `lifetime_refcount`
(`thrd_signal_handle_common.ipp.ipp`),
initialised to 1 at creation (the map's reference). A raise takes its own reference on
the container before the first unlocked decider call and releases it afterwards, and
`siguninstall` drops only the map's reference — the container is freed by
`sighandler_info_release()` when the last reference goes away (also draining
`deferred_frees`). The same refcount protocol is applied to the Windows vectored handler
(`thrd_signal_handle_windows.c.ipp`), fixing W4/2.15's container UAF. (See 8.6 for the
caveat this protocol inherits: a decider that never returns leaks the references it
took.) **Verified:** the
new regression test `test/siguninstall_raise_test.c` (registered as
`siguninstall_raise_test`) reproduces the ASan `heap-use-after-free` READ at
`thrd_signal_handle_posix.c.ipp:355` on the unfixed library (decider node retired via
`signal_decider_destroy` then container freed by `siguninstall` while the raise is inside
the decider call) and now passes; full `ctest` suite (14 tests) passes under
ASan/UBSan on macOS arm64.

### 2.3 `install_sighandler` increments `sighandlers_count` before checking TSS creation **[FIXED 2026-08-10]**

`thrd_signal_handle_common.ipp.ipp:315-323`: if `sig_global_tss_state_create()` fails
(fallback path: `calloc` failure), the function returns false but `sighandlers_count` was
already incremented and the map entry already installed. The caller (`siginstall`) treats
failure as fatal and returns NULL, leaving a handler installed that can never be
uninstalled (no handle); a subsequent `siguninstall` would call
`sig_global_tss_state_destroy()` on a TSS that was never created (see also 2.6).

**Fixed in `install_sighandler` (`thrd_signal_handle_common.ipp.ipp:355-375`):** on
`sig_global_tss_state_create()` failure the install is rolled back under the lock before
returning false: `sighandlers_count` is restored, the just-incremented `install_count` is
decremented, and if it reaches zero the handler is uninstalled
(`uninstall_sighandler_impl`, which on Windows also removes the just-added vectored
handler), the map entry erased and the container released. The forward declaration of
`uninstall_sighandler_impl` was moved before `install_sighandler` to allow the rollback.
**Verified:** no deterministic test is feasible for this OOM corner (the failure requires
the fallback-TSS `calloc` to fail); the change is confined to the first-install failure
path and the full `ctest` suite (14 tests) passes under ASan/UBSan, including the
install/uninstall cycles in `thrd_signal_handle_test`, `decider_mixed_set_test`,
`siguninstall_raise_test` and `standalone_setup_test`.

### 2.4 `sig_global_tss_state_destroy` contains dead code **[FIXED 2026-08-10]**

`thrd_signal_handle_common.ipp.ipp:276-281`: the intended reset of the static TSS slot to
NULL sits after a `return` and never runs. After a full uninstall, `*sig_tss_state_raw()`
dangles (freed TSS). **Correction (C18):** the sequential post-uninstall UAF this causes
(Z3, verified) and the concurrent-uninstall hazard (V7) are two sides of the same missing
reset.

**Fixed in the fallback `sig_global_tss_state_destroy` (`thrd_signal_handle_common.ipp.ipp`):** the
`*sig_tss_state_raw() = NULL` reset now executes after the destroy (the return value is
captured first), so a subsequent `sig_global_tss_state_init` recreates the TSS instead of
`thread_init`-ing the freed handle — this fixes the purely sequential Z3 (2.22); V7's
concurrent-uninstall framing remains open (3.16). **Verified:** the new regression test
`test/post_uninstall_reentry_test.c` (registered as `post_uninstall_reentry_test`) performs
10 install -> use -> full-uninstall -> `stdc_raise(0, ...)` + `sigguarded(...)` cycles; on
the unfixed fallback path it reproduces the ASan `heap-use-after-free` READ at
`tss_async_signal_safe.c.ipp:190` (`sig_global_tss_state_init` -> `thread_init` on the freed
handle) and now passes; full `ctest` suite (15 tests) passes under ASan/UBSan on macOS
arm64. The test is cross-platform: `INSTALLED_SIGNAL` is `SIGILL`, which is SEH-mapped on
Windows (`EXCEPTION_ILLEGAL_INSTRUCTION`) and a standard POSIX signal. The raise-while-
installed step is claimed by a global decider because on Windows an *unclaimed*
`stdc_raise` terminates the process (W5; a bare `signal()` handler is not enough — the
CRT never converts the software-raised exception, cf. `thrd_sigfpe_test.c` test 2).
SIGTERM/SIGUSR2 were rejected: SIGTERM is not SEH-mapped and
`win32_exception_code_from_signal` aborts on it.

### 2.5 `tss_async_signal_safe_thread_init` returns success when `create` yields NULL

`tss_async_signal_safe.c.ipp:188-193`: a `create` callback that returns 0 but leaves
`*dest` NULL makes `thread_init` return success (0) without inserting the TID into the
map, so a later `tss_async_signal_safe_get` on that thread returns NULL — the failure
is indistinguishable from success.

### 2.6 `tss_async_signal_safe_destroy(NULL)` dereferences NULL

The fallback `sig_global_tss_state_destroy` (2.4) and user calls with a NULL/zeroed
handle crash: `tss_async_signal_safe_destroy(NULL)` -> `LOCK(mem->lock)` on NULL. There
is no validation of the handle in any of create/destroy/thread_init/get. **Extended by
Z8:** a second `destroy` on the same handle locks freed memory, and post-destroy
`get`/`thread_init` crash identically; **extended by X8:** `tss_async_signal_safe_create`
validates neither `val` nor `attr` (`attr == NULL` -> `memcpy` crash, `attr->create ==
NULL` crashes later in `thread_init`).

### Y3 [code-level, both backends, Low] `tss_async_signal_safe_destroy` frees `mem` while still holding `mem->lock` **[FIXED 2026-08-10]**

`tss_async_signal_safe.c.ipp:111-134` does `LOCK(mem->lock)` at line 116 and never unlocks
before `free(mem)` at line 132. Harmless to the object's own lifetime (the lock word dies
with the object), but a user `attr.destroy` callback that re-enters the library on the
*same* handle (e.g. `tss_async_signal_safe_get`) spins forever on the held lock, and it is
the concrete mechanism by which a concurrent thread-exit deinit (2.1) locks a freed
object. Fix: `UNLOCK(mem->lock)` immediately before `free(mem)` (this still does not make
destroy-vs-deinit safe; it only removes the gratuitous lock state).

**Fixed:** `UNLOCK(mem->lock)` added immediately before `free(mem)` in
`tss_async_signal_safe_destroy` (`tss_async_signal_safe.c.ipp:135`); the residual
destroy-vs-live-deinit hazard is now ruled out by the destroy-after-join contract
documented in `tss_async_signal_safe.h`. **Verified:** macOS arm64 ASan/UBSan; full
`ctest` suite passes.

### 2.7 `sigguarded`/`sigfpe` NULL-argument handling aborts the process

Both backends `abort()` if `signals`, `guarded`, or `decider` is NULL
(`thrd_signal_handle_posix.c.ipp:229-233`, Windows `:227-231`). A library aborting on
argument errors is inconsistent with the rest of the API (which returns error codes) and
makes the failure mode a process crash. (The `sig_decision_invoke_recovery` decider
returning with a NULL `recovery` is legal per the docs; the abort-on-NULL applies only to
the three top-level arguments.)

### 2.8 `sigfence` on GNU compilers requires lvalues; non-lvalues fail to compile

`WG14_SIGNALS_SIGFENCE_IMPL_1(a)` expands to `__asm__ volatile(";" : "+m"(a) : : "memory")`.
The `+m` operand must be an lvalue: `sigfence(42)` or `sigfence(x + 1)` is a hard compile
error. Verified (`error: invalid lvalue in asm output`); the 0-arg form compiles.

### 2.10 V2 [code-level, Windows] `win32_vectored_exception_function` NULL-derefs the per-thread state on fresh threads (High)

`thrd_signal_handle_windows.c.ipp:357-361`: when a global decider returns a claiming
decision, the handler calls `sig_global_tss_state()` and immediately dereferences
`tss->front`. The per-thread TLS state is created only by `sig_global_tss_state_init()`
(a prior `sigguarded`/`stdc_raise` on that thread); the vectored handler never
initialises it. A genuine fault (AV, div-by-zero) on a thread that has only ever called
`siginstall` (or nothing) -> `tss == NULL` -> NULL dereference *inside the exception
handler*, turning a recoverable fault into a crash. POSIX is immune (its handler inits
the TLS state as part of the raise). **Extended by X3:** a Windows thread whose only
library interaction is `sigguarded()` (never `stdc_raise`) also has a NULL per-thread
state, so `sigguarded` alone is insufficient on Windows while being sufficient on POSIX.

### 2.11 V3 [code-level, Windows] Unsupported exception codes reach `sigismember(guarded, 0)` -> UB; C++ exceptions can be swallowed by `sigguarded` (High)

`win32_exception_filter` (`:194-217`) evaluates `sigismember(guarded, signo)` where
`signo = signal_from_win32_exception_code(GetExceptionCode())` — 0 for every unsupported
code (C++ exceptions `0xE06D7363`, third-party `RaiseException`s, CRT codes). The Windows
inline `sigismember` computes `1u << (signo - 1)` = `1u << -1`: undefined behaviour. For a
`sigfillset`-built guard set (bit 31 set) the guard claims "signal 0", the user decider
runs with a garbage `rsi`, and if it returns `invoke_recovery` with a recovery function
the `__except` body runs and **the foreign exception is swallowed** — in C++ an exception
thrown inside `sigguarded` (e.g. `std::bad_alloc`) is caught instead of propagating. The
filter must range-check `signo >= 1` before the membership test.

### 2.12 V4 [code-level, Windows] `stdc_raise` aborts for every unsupported signo (High)

`win32_exception_code_from_signal` handles only SIGABRT/SIGBUS/SIGILL/SIGSEGV/SIGFPE;
`stdc_raise(SIGINT)`, `SIGTERM`, `SIGPIPE`, `SIGUSR1` etc. all hit `default: abort()`.
On POSIX the same calls are harmless no-ops when no decider is installed. The header
documents `stdc_raise` as usable for "OUR currently installed signal decider" for
arbitrary signals.

### 2.13 W1 [code-level, fallback TLS path] `tss_async_signal_safe_thread_init` leaves a dangling map entry when the `deinit_state` allocation fails (High — OOM corner) **[FIXED 2026-08-10]**

`tss_async_signal_safe.c.ipp:201-213`: when `calloc` of `mem->state` fails, the failure
path destroys `newitem` but **does not erase** the `[mytid] -> newitem` entry committed
just before the allocation. The map now holds a dangling pointer: the next `get` on that
thread returns the freed pointer (UAF on read), and the next `thread_init` finds the
entry, skips the create, and reports success — the thread is permanently bound to freed
storage, `count` is never incremented, and no atexit cleanup is registered. Fix: erase
the map entry before destroying `newitem` (or move the `state` allocation before the
`insert`).

**Fixed:** the OOM path now erases the `[mytid]` entry before `UNLOCK`/destroying
`newitem` (`tss_async_signal_safe.c.ipp:217-221`). **Verified:** macOS arm64 ASan/UBSan;
full `ctest` suite passes.

### 2.14 W2 [confirmed, POSIX] deciders receive indeterminate/stale `error_code`, `addr`, `raw_info` for `stdc_raise(signo, NULL, NULL)` (High)

`prepare_rsi` (`thrd_signal_handle_posix.c.ipp:186-199`) writes `raw_info`,
`error_code`, and `addr` **only** when `siginfo != NULL`. On the global path `rsi` is a
fresh uninitialised stack variable (`:327`) — all three fields are indeterminate garbage
(reproduced); `raw_info` is a garbage *pointer* the decider may dereference. On the frame
path the frame's `rsi` is zeroed at `sigguarded` entry, but a *second* raise in the same
frame with NULL `info` keeps the stale `raw_info` pointer from the first raise — a
pointer into a dead kernel stack frame. The documented usage `stdc_raise(signo, nullptr,
nullptr)` is exactly what the tests and README use. Pass 2's 7.2 only noted `value`
(C8); the defect is broader. Fix: memset the struct in `prepare_rsi` regardless of
`siginfo`. **Correction (C14):** a decider returning `next_decider` on a *user* raise
falls through to the default action and the process dies (default-terminate signals) —
on POSIX an unclaimed user raise of a default-terminate signal kills the process, the
same outcome Windows produces via WER (W5). POSIX only "returns false" when the signal
was never installed.

### 2.15 W4 [code-level, Windows] the 2.2 container UAF exists in the Windows vectored handler too (High) **[FIXED 2026-08-10]**

`win32_vectored_exception_function` (`thrd_signal_handle_windows.c.ipp:329-368`) holds
`it` across the unlocked `current->decider(&rsi)` call and re-accesses
`signo_to_sighandler_map_t_value(it)->global_handler` after re-locking. A concurrent
`siguninstall` that drops the last reference frees the `sighandler_info` container in
that window -> UAF *inside the exception handler*. Additionally the vectored handler may
be *removed* mid-raise when the count reaches zero, so the remainder of the raise runs
with no library filter at all.

**Fixed:** the same 2.2 refcount protocol — the vectored handler takes a reference on the
`sighandler_info` container across the unlocked decider calls and releases it afterwards
(`thrd_signal_handle_windows.c.ipp:345-397`), so a concurrent `siguninstall` cannot free
the container while the handler is inside a decider call. The filter-removal-mid-raise
behaviour is unchanged (a semantic issue, not a memory-safety one). **Verified:** Windows
path shares the identical code pattern fixed and regression-tested on POSIX
(`test/siguninstall_raise_test.c`); no Windows host available for live validation.

### 2.16 W5 [code-level, Windows] `stdc_raise` can never return `false`; an unclaimed raise terminates the process (High — documented-contract violation)

`thrd_signal_handle_windows.c.ipp:252-300` raises via `RaiseException` and always returns
`true`. The header contract (`thrd_signal_handle.h:362-364`) promises "returning false if
we have no decider installed for that signal". On Windows, a supported signo with no
installed handler and no guard reaches the library's own `UnhandledExceptionFilter`
(which returns `EXCEPTION_CONTINUE_SEARCH`) and Windows Error Reporting **terminates the
process**; the same happens inside a `sigguarded` frame guarding a *different* signal.
So the portable idiom `if(!stdc_raise(signo, ...)) { /* fall back */ }` is deadly on
Windows. Distinct from V4 (which covers `abort()` on unsupported signos); this is the
"no decider" path for *supported* signos.

### 2.17 X1 [confirmed, High] `tss_async_signal_safe_thread_init` performs a use-after-free on the `deinit_state` when called after all previously registered threads have exited **[FIXED 2026-08-10]**

`tss_async_signal_safe.c.ipp:162-166` — the last thread's deinit runs `free(state)` on the
shared `deinit_state`, but **nothing clears `mem->state`**. A later `thread_init` on a
fresh thread (a completely ordinary sequential pattern: create -> T1 inits+exits -> T2
inits) finds `mem->state` still non-NULL (dangling), skips the allocation at `:201-204`,
and executes `atomic_fetch_add(&mem->state->count, 1)` at `:214` on freed memory.
Reproduced with ASan (`heap-use-after-free` WRITE at `:214`). The `thread_atexit`
registration then references the freed `state`, so the new thread's own exit runs the
deinit on freed memory again. The fallback TLS path is affected; the async-safe TLS path
never uses this object. Fix direction: the last deinit must clear `mem->state` (under
`mem->lock`) before `free(state)`, or `thread_init` must validate `mem->state` against a
generation/refcount.

**Fixed:** the last deinit now clears `mem->state` (under `mem->lock`, guarded by
`state == mem->state`) before `free(state)`, so the next `thread_init` reallocates a fresh
`deinit_state` (`tss_async_signal_safe.c.ipp:168-176`). **Verified:** the regression test
`test/tss_concurrent_exit_test.c` (`test_reinit_and_destroy_after_all_exited`) produced
the ASan `heap-use-after-free` WRITE at `:217` on the unfixed library and now passes
(macOS arm64, ASan/UBSan).

### 2.18 X2 [confirmed, High] `tss_async_signal_safe_destroy` writes through the dangling `mem->state` when all registered threads exited before destroy **[FIXED 2026-08-10]**

Same root cause as X1, different trigger: when the last registered thread already exited
(`state` freed at `:165`), a later `tss_async_signal_safe_destroy(val)` executes
`mem->state->val = NULL` at `:119` on freed memory. Reproduced with ASan (WRITE of size 8
at `:119`). Note the documented destroy-after-join pattern ("destroy only after all
threads left") is exactly the pattern that hits this. Both X1 and X2 are reachable
through the public `tss_async_signal_safe_*` API alone.

**Fixed:** the same X1 change — the last deinit clears `mem->state` before freeing
(`tss_async_signal_safe.c.ipp:168-176`) — so a destroy that follows the last thread's
exit finds `mem->state == NULL` and skips the write; the destroy-after-join requirement is
documented in `tss_async_signal_safe.h`. **Verified:** the destroy-after-last-exit path is
covered by `test/tss_concurrent_exit_test.c` (macOS arm64, ASan/UBSan), and the full
`ctest` suite passes.

### 2.19 X3 [code-level, Windows, Medium-High] `sigguarded` on Windows never initialises the per-thread TSS (unlike POSIX)

`thrd_signal_handle_windows.c.ipp:219-249` — the Windows `sigguarded` performs no
`sig_global_tss_state_init()` call; POSIX does (`thrd_signal_handle_posix.c.ipp:234`). A
Windows thread whose only library interaction is `sigguarded()` has a NULL per-thread
state; if a genuine fault then occurs and a global decider claims it, the vectored
handler evaluates `tss->front` on the NULL state (`:357-361`) — a crash *inside the
exception handler*. Extends V2.

### 2.20 Y1 [confirmed, High] `install_sighandler` leaks the global spinlock when `install_sighandler_impl` fails — a second instance of the global-spinlock-leak bug family (both backends)

`thrd_signal_handle_common.ipp.ipp:305-311`: when the backend installation fails (POSIX
`sigaction` error; Windows `AddVectoredContinueHandler` returning NULL at
`thrd_signal_handle_windows.c.ipp:417-422`), `install_sighandler` returns `false` with
`state->lock` held forever. Every subsequent library call and any signal delivery
through `raw_signal_handler` spins forever. Verified: with `install_sighandler_impl`
forced to fail, the first `siginstall(NULL)` returns NULL and the second call busy-spins
at ~99% CPU. On POSIX the path is effectively dead (`sigaction` cannot fail for the
signals the loop visits), but on Windows `AddVectoredContinueHandler` failure is a rare
resource-exhaustion event, converting a recoverable failure into a permanent
whole-library deadlock. Fix: `UNLOCK(state->lock)` before the `return false`.

### 2.21 Z1 [confirmed standalone, Medium-High] the verstable-variant `signo_to_sighandler_map_t` is never initialised — NULL `metadata` dereference on every library operation (NSIG >= 1024 platforms)

`thrd_signal_handle_common.ipp.ipp:58-135` selects the verstable hash-map variant of
`signo_to_sighandler_map_t` when `NSIG >= 1024`. Unlike `thread_id_to_tls_map_t` (which
`tss_async_signal_safe_create` initialises), no call to `signo_to_sighandler_map_t_init`
exists anywhere (grep confirmed). `sig_global_state()` returns a zero-initialised
function-local `static` struct, so for the verstable variant `buckets == NULL` and
`metadata == NULL`, and `_get`/`_insert` dereference `metadata[0]` immediately. Reproduced
standalone (SIGSEGV). Every map-touching operation crashes on such a platform. Mainstream
libcs never reach NSIG >= 1024, so no CI leg exercises this branch. Fix: call
`signo_to_sighandler_map_t_init` once.

### 2.22 Z3 [confirmed, High, fallback-TLS platforms] every `sigguarded`/`stdc_raise` after a full `siguninstall` is a heap-use-after-free on the destroyed TSS handle (extends 2.4 / V7) **[FIXED 2026-08-10]**

`thrd_signal_handle_common.ipp.ipp:276-281`: `sig_global_tss_state_destroy`'s intended
reset of `*sig_tss_state_raw()` to NULL is dead code. After the final `siguninstall` on
the fallback path, `tss_async_signal_safe_destroy` frees the shared object but the static
slot still points at it. Verified with ASan: `stdc_raise(0, NULL, NULL)` and
`sigguarded(...)` after an install->uninstall cycle give a heap-use-after-free READ at
`tss_async_signal_safe.c.ipp:177`; a real signal delivered to a thread that re-enters the
library after a second `siginstall` also hits the dangling handle (only a fresh
`siginstall` overwriting the slot restores sanity). This is the purely sequential,
documented-usage failure; V7's concurrent framing and 2.4's dead-code framing are two
sides of the same missing reset (C18). Fix: reset the slot to NULL on destroy, and guard
`thread_init` against a NULL handle.

**Fixed:** the slot reset is now executed (2.4 fix), so every documented sequential
re-entry after a full `siguninstall` recreates the TSS — both ASan-verified triggers
(`stdc_raise(0, ...)` and `sigguarded(...)`) pass in the new `post_uninstall_reentry_test`.
The `thread_init` NULL-handle guard is tracked separately (2.5/2.6); V7's concurrent
uninstall remains open (3.16).

### 2.23 AA1 [confirmed, Medium-High] `signal_decider_destroy` crashes after a `siguninstall` -> `siginstall` cycle orphans the decider node (sequential, no concurrency)

`thrd_signal_handle_common.ipp.ipp:443-614`. `siguninstall` (via `uninstall_sighandler`,
`:328-364`) frees the per-signal `sighandler_info` container as soon as its refcount hits
zero, but it does **not** look at the container's `global_handler` list — any decider
node still linked there survives with `prev`/`next` pointing into freed memory. A later
`signal_decider_destroy(handle)` for that decider then finds the *reinstalled* signal's
new container, decrements the node's refcount to 0, and executes `LIST_REMOVE` against
the new (empty) list using the orphaned node's stale pointers — a NULL-pointer write
(verified: ASan SEGV, WRITE to 0x0, at `:590`); with multiple orphaned nodes the
neighbour pointers point into the freed container -> heap-use-after-free instead. The
exact crash sequence is all public, documented API:

```c
h  = siginstall(&g);                  // g = {SIGUSR1}
d  = signal_decider_create(&g, ...);  // node linked into SIGUSR1's global_handler
siguninstall(h);                      // frees the container; node orphaned
h2 = siginstall(&g);                  // new container for SIGUSR1
signal_decider_destroy(d);            // LIST_REMOVE on orphaned node -> crash
```

Without the reinstall, `signal_decider_destroy` still frees the orphaned node cleanly.
This is the sequential sibling of the concurrent container UAF (2.2/W4): the common root
cause — `uninstall_sighandler` freeing `sighandler_info` without accounting for nodes
still linked in its `global_handler` list — should be fixed once for all three (C22).
It also means `siguninstall` *silently orphans* any decider still registered for the
uninstalled signal.

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
(`thrd_signal_handle_common.ipp.ipp:206-227`, `thread_atexit.cpp.ipp:57-64`). If the
first signal ever delivered to a thread arrives before any library call on that thread,
malloc and C++ heap operations run inside the handler (not async-signal-safe; risk of
deadlock on the heap lock). The docs recommend pre-calling `stdc_raise(0, ...)`; on Linux
this works, but the safety relies entirely on the user reading the docs.

### 3.3 `SA_NOCLDWAIT` + `SA_NODEFER` + missing `SA_RESTART` alter process semantics

`install_sighandler_impl` (`thrd_signal_handle_posix.c.ipp:371-383`) installs with
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

`thrd_signal_handle_posix.c.ipp:143-184`: the "default is to ignore" list only covers
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

`thrd_signal_handle_posix.c.ipp:203-219`: if `stdc_raise` returns false, the handler
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
with stale state. There is no TID-generation counter. **Extended by Y4:** when the user's
`attr.destroy` returns non-zero, `tss_async_signal_safe_thread_deinit`
(`tss_async_signal_safe.c.ipp:153-158`) returns early with the TID entry still in the map
and `state->count` not decremented (the `fetch_sub` is after the early return, so the
count never reaches the free point for that thread) — nobody else erases the TID, the
failed callback never re-runs, and the count stays permanently off relative to the live
threads. **Y4 [FIXED 2026-08-10]:** the deinit rewrite
(`tss_async_signal_safe.c.ipp:139-180`) no longer returns early on a non-zero
`attr.destroy` result: the TID entry is still erased and `state->count` still
decremented, so the bookkeeping stays consistent (only the failed-to-destroy per-thread
value leaks, which is the callback's fault). **Verified:** macOS arm64 ASan/UBSan; full
`ctest` suite passes.

### 3.7 `sig_global_state_tss_state_init` failure inside `stdc_raise` hides the real error

`stdc_raise` returns `false` both for "no handler installed for this signal" and for
"TSS init failed" (`thrd_signal_handle_posix.c.ipp:277-285`). The POSIX `signo == 0`
setup call also returns false on init failure, so the documented setup call gives no
diagnostic when setup actually failed.

### 3.8 Partial install failure in `siginstall` leaves handlers installed (no rollback)

`thrd_signal_handle_common.ipp.ipp:381-405`: if `install_sighandler` fails for any signal
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

`thrd_signal_handle_common.ipp.ipp:603-608`: after decrementing a node's refcount to zero
under the lock and removing it from the list, `free(*retp)` runs after `UNLOCK`. In
practice the refcount increment precedes the unlocked decider call, so the current design
is safe — **confirmed by a fresh trace (C21):** the base refcount is 1 (create), an
in-flight raise increments to 2 *before* the unlocked decider call, and `destroy`'s
`--refcount` then takes the deferral branch; the raise's later `--refcount` reaches 0 and
moves the node to `deferred_frees`. The concurrent-destroy test genuinely exercises this
deferred path. Still, the free outside the lock is fragile and undocumented.

### 3.11 `thread_atexit` C++ exceptions disabled -> OOM terminates

`thread_atexit.cpp.ipp:57-71`: with `-fno-exceptions` the `try/catch` block is compiled
out; `std::vector::emplace_back` on allocation failure calls `std::terminate` instead of
returning -1. The library is designed to be embedded in C standard libraries where
exceptions may be disabled; this path then crashes instead of reporting failure.

### 3.12 Function-pointer type pun for atexit callback

`tss_async_signal_safe.c.ipp:217-218` casts `int (*)(struct deinit_state *)` to
`void (*)(void *)` and registers it via `thread_atexit`. Calling through an incompatible
function-pointer type is UB per the C standard (works on common ABIs, but a latent
portability hazard).

### 3.13 `tss_async_signal_safe_thread_init` re-entrancy (signal during `attr.create`) leaks

`tss_async_signal_safe.c.ipp:184-191` unlocks before calling the user's `create` and
re-locks before `insert`. If a signal handler runs `thread_init` on the same object in
that window (possible via `stdc_raise` -> `sig_global_tss_state_init` -> `thread_init`),
the user's `create` callback runs twice and the second `insert` replaces the first entry:
the first value leaks (no destructor on replace), `count` is double-incremented, and two
atexit registrations are queued.

### 3.14 `tss_async_signal_safe_thread_init` does not roll back on `thread_atexit` failure

`tss_async_signal_safe.c.ipp:214-223`: the map entry and `state->count` are committed
before `thread_atexit` is called; if it returns -1 the caller sees failure but the entry
and count remain, and no thread-exit cleanup will ever run for this thread. (On the
async-safe TLS path, `sig_global_tss_state_init` has the same pattern — it sets `*state =
mem` *before* `thread_atexit(free, mem)`; on registration failure the pointer stays set
and the next call silently succeeds with a leaked `mem`, M2.)

### 3.15 V5 [code-level, Windows, Medium] Global deciders can be invoked two or three times per single exception

The same function is registered both as `AddVectoredContinueHandler` and as the
unhandled exception filter (`install_sighandler_impl`, `:415-424`). **Correction (C15/C16):**
with no `AddVectoredExceptionHandler`, the effective dispatch order is frames-first:
frame `__except` filters (frame deciders) -> unhandled filter (global deciders) ->
continue handler (global deciders again). The library function runs at most **twice** per
exception, and only **once** under a debugger (the unhandled filter is not invoked under
a debugger). So the side-effecting-decider double-run claim stands for the no-debugger
path when no decider claims; the pass-5 debate over exactly one vs two invocations on the
no-debugger path (C19) was left unresolved — the Windows CI's
`thrd_signal_handle_test` asserting `count_decider == 1` for a *claiming* decider passes,
consistent with the claiming path running once.

### 3.16 V7 [code-level, fallback path, Medium] `siguninstall` of the last handler while another thread is inside `sigguarded` frees that thread's live state

On the fallback (Apple) path, the final `siguninstall` -> `sig_global_tss_state_destroy`
-> `tss_async_signal_safe_destroy` frees the shared TSS (and every per-thread entry)
*while another thread may be between `sigguarded` frames*. The interrupted thread's
`tss->front` points into the freed per-thread state; its next raise runs
`sig_global_tss_state_init` against the *dangling* `*sig_tss_state_raw()` (2.4/Z3) -> UAF.
Only the fallback path is affected (the async-safe TLS path's destroy is a no-op). The
usage requirement "destroy only after all threads have left `sigguarded`" is nowhere
documented.

### 3.17 W3 [code-level, POSIX, Medium] nested signal delivery during a decider call races on the shared frame `rsi`

`thrd_signal_handle_posix.c.ipp:294-295` — the frame's `rsi` is both written
(`prepare_rsi`) and read (`frame->decider(&frame->rsi)`) from the same thread. With
`SA_NODEFER`, a second delivery of a guarded signal while the first decider is still
executing re-enters `raw_signal_handler` -> `stdc_raise` -> `prepare_rsi` on the **same**
`frame->rsi`, overwriting the fields mid-decider: the outer decider reads torn fields, and
if the nested raise chooses `invoke_recovery` it `longjmp`s out of the outer decider —
the *outer* recovery runs with the *nested* `rsi` contents. Not a C memory-model race
(same thread), but a re-entrancy aliasing bug; on Windows the frame `rsi` is a fresh
local in `win32_exception_filter`, so nested exceptions cannot corrupt it.

### 3.18 X4 [code-level, Windows, Medium] `EXCEPTION_STACK_OVERFLOW` (0xC00000FD) is not mapped to any signal

`signal_from_win32_exception_code` (`thrd_signal_handle_windows.c.ipp:135-164`) covers
the five raised signals plus the eight `EXCEPTION_FLT_*`/`EXCEPTION_INT_*` codes, but has
no case for `EXCEPTION_STACK_OVERFLOW`. A genuine stack overflow returns `signo == 0` ->
`EXCEPTION_CONTINUE_SEARCH` -> WER terminates the process with no library involvement,
whereas on POSIX a stack overflow delivers `SIGSEGV`, which *is* handled. A real
functional gap between backends for a common, otherwise-recoverable fault class, and it
is not documented.

### 3.19 Z2 [confirmed, Low-Medium] `stdc_raise(signo, NULL, NULL)` hands off to a pre-existing `SA_SIGINFO` handler with NULL `siginfo_t *` and NULL `ucontext_t *`

`thrd_signal_handle_posix.c.ipp:314-368`: when no frame or global decider claims the
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

`tss_async_signal_safe.c.ipp:184-191`: the user's `create` callback runs **without**
`mem->lock`, so two threads racing to first-initialise the same handle call the user's
`create` concurrently. The API documents `thread_init` as THREADSAFE and serialises the
map insert but not the create callback. The test suite's own `create` uses a shared
`static unsigned *storage_ptr`, so a two-worker concurrent first-use would be a data race
in the harness itself. Design note: either document that `create` must be thread-safe, or
serialise the create callback under the lock (at the cost of re-entrancy, cf. 3.13).

---

## 4. Portability and configuration concerns

### 4.1 Default `get_current_thread_id` fallback is FreeBSD-only

`current_thread_id.c.ipp:70-72`: the final `#else` branch calls
`pthread_getthreadid_np()`, which exists only on FreeBSD (the FreeBSD block also pulls in
`<pthread_np.h>` at line 36-38). On any other POSIX platform the code fails to compile.
The portable fallback should be `(thread_id_t)pthread_self()`.

### 4.2 `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` auto-detection is too optimistic

`config.h:41-51` enables async-safe TLS for *any* `__GNUC__` (which includes clang) on
any non-Apple platform. This is only true where the toolchain actually supports
`tls_model("initial-exec")` and the libc reserves static TLS for dlopened libraries.
Also, a user-defined `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on a compiler that is
neither `__GNUC__` nor `_MSC_VER` leaves `WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL` undefined
while code references it -> compile error with no diagnostic.

### 4.3 `pthread_getthreadid_np` / `mach_thread_self` are not async-signal-safe

`current_thread_id.c.ipp:66-69`: the Apple branch performs `mach_port_deallocate` (kernel
round-trip) on every cache miss; `current_thread_id()` is documented as "ASYNC SIGNAL
SAFE" (`current_thread_id.h:57`). On fallback platforms the cache miss happens inside a
signal handler on first use -> async-signal-unsafe syscalls.

### 4.4 MSVC CRT signals bypass SEH (Windows)

`siginstall` on Windows installs only SEH vectored handlers
(`thrd_signal_handle_windows.c.ipp:408-427`); it does not install CRT signal handlers.
`abort()`, `raise(SIGFPE)`, `assert`, etc. on MSVC dispatch through the CRT, which does
not raise SEH exceptions — so they never reach the library. Only genuinely SEH-raised
exceptions (access violations, integer overflow traps on x86, explicit `RaiseException`)
are handled. A substantial functional gap on Windows versus POSIX, and it is not
documented.

### 4.5 32-bit sigset_t on Windows overflows for signals >32

`thrd_signal_handle.h:52-63`: `sigaddset` computes `1u << (signo - 1)` on `uint32_t`.
Any signal number >32 (e.g. realtime 34-64) is undefined behaviour. Currently only
numbers up to 22 are used, but the header's comment claims "MSVC appears to follow the
Linux signal numbering" — with `SIGSYS`(31) this is just inside the limit; realtime
signals would overflow.

### 4.6 Missing `SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards

`thrd_signal_handle_posix.c.ipp:53-54` uses `SIGSYS`, and `:109` uses `SIGXCPU`/`SIGXFSZ`
without `#ifdef` guards (only `SIGPOLL` is guarded). On a POSIX platform that omits any of
these the file fails to compile.

### 4.7 `ucontext_t` and `siginfo_t` portability

`thrd_signal_handle.h:202-216` relies on `<signal.h>` defining `ucontext_t` (POSIX does
not require this; `<ucontext.h>` does) and uses platform-specific spellings
(`struct __siginfo`, `struct siginfo`) that assume BSD/Android/glibc layouts.

### 4.8 Mingw

Deliberately unsupported (`#error` at `thrd_signal_handle_windows.c.ipp:233-235`), but
before reaching that `#error` the header has already redefined `sigset_t` on `_WIN32`
(`thrd_signal_handle.h:43`), which collides with MinGW's own `sigset_t` typedef — the
first of several Mingw incompatibilities.

### 4.9 `_setjmp` vs `setjmp` inconsistency for header-only consumers

`WG14_SIGNALS_HAVE__SETJMP` is set only on the compiled library target
(`CMakeLists.txt:32-34`, PRIVATE). Header-only consumers never get the definition and
always use `setjmp` (saving/restoring the signal mask) even when `_setjmp` is available.
Not a correctness bug but a silent performance/behaviour split between the two modes.

### 4.10 `__VA_OPT__` dependency

`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:86`) requires `__VA_OPT__`
(C23 / C++20, or the GCC>=8 / Clang>=12 extension). Verified: GCC and Clang accept it
silently in C11 mode with `-Wpedantic`, so this works on current toolchains, but it is a
latent portability break for older compilers or strict MSVC C modes.

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
`current_thread_id.c.ipp:50-56`) and `my_current_thread_id`
(`tss_async_signal_safe.c.ipp:81-91`) holding the *parent's* TID — every
`current_thread_id()`/`tss_async_signal_safe_get()` in the child returns the wrong
identity for the child's lifetime — plus the copied `thread_id_to_tls_map` and
`sig_global_state`, so the child's map lookups keyed by the stale parent TID can return
the *parent's* per-thread values. For a library whose stated purpose is thread-local
signal handling inside a C runtime, fork-safety is a realistic requirement and is neither
implemented nor documented.

### 4.14 Y6 [code-level, Low] `NSIG` is not POSIX-mandated; a missing `NSIG` silently disables `siginstall`

`thrd_signal_handle_common.ipp.ipp:58-62` uses `#if NSIG < 1024` (undefined `NSIG`
evaluates to 0), and the `siginstall`/`siguninstall`/decider loops iterate `1 .. NSIG-1` —
with `NSIG` undefined the loops never execute and `siginstall` **returns success having
installed nothing**. All CI platforms define NSIG, so this is exotic-POSIX-only, but the
failure is silent.

### 4.15 Z4 [code-level, Low, extends X7/V4] negative `signo` in `stdc_raise` is UB on the POSIX frame walk and `abort()`s on Windows

`thrd_signal_handle_posix.c.ipp:292`: `sigismember(frame->guarded, signo)` with a negative
`signo` expands (macOS/BSD macro form) to `1u << (signo - 1)` — a negative/oversized shift
count, i.e. UB, whenever any frame exists. On Windows, `stdc_raise(-1, ...)` ->
`win32_exception_code_from_signal` `default: abort()`. Three platforms give three
different behaviours for the same invalid input.

### 4.16 X7 [code-level, Low] out-of-range `signo` reaches `sigismember` without bounds checks (UB on BSD/macOS)

`thrd_signal_handle_posix.c.ipp:292` — the frame loop tests
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
compiled nor linked — the library is all-C with no C++ runtime dependency there.)

### 5.3 CMake `CMAKE_C_STANDARD` cache variable is unused for consumers and the header-only test lacks `-Werror`

The library compiles with `-Werror` (`CMakeLists.txt:47`) but the tests and the
`header_only_test` target do not (`test/CMakeLists.txt:12`), so warnings that would break
a strict build are invisible.

### 5.4 CI gaps

- No CI runs with `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` on Linux (the fallback
  path is only exercised on macOS).
- The Windows CI runs with `-DCMAKE_C_STANDARD` in {11,17} but MSVC ignores the C-standard
  option for `/experimental:c11atomics` in some versions.
- No CI runs the benchmark targets at all (`-E benchmark`), so the performance claims in
  the README are not verified.
- No CI tests C11 atomics with an actual stress/TSan build; the spinlock code paths (3.1)
  would benefit from TSAN. **[FIXED 2026-08-10]** — a dedicated `TSan` job (Ubuntu
  gcc/clang and macOS clang, C11 and C23) was added to `.github/workflows/ci.yml`, driven
  by `cmake/tsan-toolchain.cmake` (`-fsanitize=thread` on C/CXX and the linker) with
  `TSAN_OPTIONS=halt_on_error=1 log_path=stderr symbolize=1 history_size=7`. Because
  glibc's `thrd_create()` calls `pthread_create()` inside libc and bypasses TSan's
  interceptor, `test/test_common.h` now selects the pthread-based `thrd_*` shim under
  glibc+TSan (ideas.md 6.2). Verified on macOS arm64 (clang 17): all 15 `ctest` tests
  pass under TSan in C11 and C23 builds. This supplies the race-free verification of the
  2.1/2.2 fixes (the macOS legs exercise the fallback TLS path); 3.1 (spinlock not
  async-signal-safe) is a handler-re-entrancy hazard TSan cannot detect and remains open.

### 5.5 `ProjectConfig.cmake.in` references non-existent export names

`cmake/ProjectConfig.cmake.in:6-11` conditionally includes
`@PROJECT_NAME@SlExports.cmake`/`@PROJECT_NAME@DlExports.cmake`, which are never
generated. Harmless (guarded by `EXISTS`), but misleading.

### 5.6 V1 [confirmed] Installed package is unusable: no headers installed, `find_package` hard-fails (Critical, packaging)

`CMakeLists.txt` has no `install(DIRECTORY include/ ...)` rule — `cmake --install`
produces only `lib/libwg14_signals.a` and `lib/cmake/wg14_signals/*`. Additionally,
`ProjectConfig.cmake.in` is processed with plain `configure_file(... @ONLY)` instead of
`configure_package_config_file()`, so `@PACKAGE_INIT@` never receives its definition:
CMake >= 4.0 substitutes the undefined `@VAR@` with an empty string, so the installed
config loses the `PACKAGE_INIT` macro block and `check_required_components(wg14_signals)`
errors out ("Unknown CMake command"); CMake < 4.0 leaves the literal `@PACKAGE_INIT@` in
the installed file and fails as an unknown command on include. Either way the documented
consumption path is broken, and even after fixing the config the exported target's
`INTERFACE_INCLUDE_DIRECTORIES` points at `<prefix>/include`, which does not exist. Pass 1
(5.5) only noted the harmless `SlExports`/`DlExports` references and missed this.

### 5.7 W6 [build, Medium] `PROJECT_IS_TOP_LEVEL` requires CMake >= 3.21 while `cmake_minimum_required` is 3.15

`CMakeLists.txt:1` declares `cmake_minimum_required(VERSION 3.15 FATAL_ERROR)`, but line
72 gates the entire `test/` subdirectory on `PROJECT_IS_TOP_LEVEL`, a variable introduced
in CMake 3.21. On CMake 3.15-3.20 the variable is undefined and the condition silently
evaluates false: no tests, no `header_only_test` target, and `BUILD_TESTING` is ignored —
with no diagnostic. Either raise the minimum to 3.21 or use
`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`.

### 5.8 W7 [build, minor] MSVC builds lack the `-Werror` equivalent

`CMakeLists.txt:44-48`: GCC/Clang get `-Wall -Wextra -Wpedantic -Werror`; MSVC gets
`/W4 /experimental:c11atomics` with no `/WX`. All warnings that would break a strict
GCC/Clang build are invisible in the Windows CI leg (extends 5.3).

### 5.9 Y7 [build, Low] the project unconditionally requires a C++ compiler for a C library

`CMakeLists.txt:11` declares `project(wg14_signals LANGUAGES C CXX)` unconditionally (only
the *chosen* `thread_atexit` source is conditional, at `:71-75`), so a C-only toolchain
cannot configure the project on any platform — even on `__cxa_thread_atexit()` platforms
where the compiled library is all-C.

### 5.10 AA2 [code-level, build-config, Low] the filc toolchain's `-DDISABLE_INLINE_ASM=1` is a no-op for the library; the toolchain also hardcodes machine-specific paths

`cmake/filc-toolchain.cmake:3-4` passes `-DDISABLE_INLINE_ASM=1` for both C and C++.
`DISABLE_INLINE_ASM` is referenced **only** by `test/ticks_clock.h:44` (the benchmark
clock); the library itself never tests it. The library's `WG14_SIGNALS_SIGFENCE_IMPL_*`
selection in `thrd_signal_handle.h:97-154` keys off `__GNUC__ || __clang__` only — filc
is clang-based, so `__clang__` is defined and the GNU inline-asm path is compiled
regardless of the flag (verified by a `-DDISABLE_INLINE_ASM=1 -fsyntax-only` build
succeeding unchanged; C23). The toolchain's apparent intent (disable inline asm under
filc) is not achieved; if filc's assembler rejects the `__asm__ volatile(";")` forms, the
library build fails. Additionally the toolchain hardcodes `/home/ned/Downloads/...`
compiler paths (not portable), and it is never exercised by CI. Fix direction: gate the
GNU inline-asm block on `#ifndef DISABLE_INLINE_ASM` (falling back to the volatile-sink
fallback) and drive the toolchain from a `FILC_ROOT` variable.

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

`test_common.h:48-59` — `thrd_join` checks `ret != -1`, but `pthread_join` returns an
error *number* (0 on success), never -1: on failure `*res` is left unset while the caller
proceeds as if the join succeeded. `thrd_create` (`:43-48`) dereferences the unchecked
`calloc` result (NULL deref on OOM). The benchmark and handle tests rely on this shim; the
harness masks real failures.

### 6.6 AA3 [confirmed, build-config, Low] `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` breaks the test suite

`thrd_signal_handle.h:77` gates the whole `sigfence` macro behind
`#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`, but `test/thrd_sigfpe_test.c:64` calls
`sigfence(result)` unconditionally. Defining the macro (a legitimate, if undocumented,
config knob) makes the test fail to compile (verified: `call to undeclared function
'sigfence'`). There is no CI configuration that tests the macro, and `config.h` does not
document it.

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

`thrd_signal_handle_posix.c.ipp:49-67` double-checks `sigismember(&v, signos[0])` then
writes `v = x` without a lock or atomics. Two threads calling `sigfillset_synchronous`
concurrently both compute `x` and both store `v` — a benign write-write race on identical
values, but still a data race under the C memory model (UB), and the "is it initialised?"
check reads a non-atomic while another thread writes. In practice the sigset write is
aligned and atomic; the sets also carry `__attribute__((constructor))` (M4), which
pre-initialises the statics at load time for executables and substantially mitigates the
race — but the attribute is POSIX-only (C9), so the race is unmitigated on Windows for
`synchronous_sigset`/`asynchronous_nondebug_sigset`.

### 7.2 `prepare_rsi` leaves `rsi->value` indeterminate on POSIX

`thrd_signal_handle_posix.c.ipp:186-199` does not initialise `rsi->value`; the global
decider loop always overwrites it (`:336`) and the frame path uses the frame's persistent
`rsi`, so no read of the indeterminate value occurs today. **Correction (C8/W2):** the
indeterminacy is not limited to `value` — `error_code`, `addr`, and `raw_info` are
indeterminate (global path) or stale (frame path, repeat raises) whenever `info == NULL`
(2.14, reproduced).

### 7.3 AA8 [code-level, Low] the `tss_async_signal_safe` per-thread ID cache uses plain `_Thread_local`, not the async-signal-safe attribute

`tss_async_signal_safe.c.ipp:81-91` declares the `my_current_thread_id()` cache with
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

`thrd_signal_handle_posix.c.ipp:240-266`: `current.rsi` is written by `prepare_rsi` (via
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

`thrd_signal_handle_posix.c.ipp:251-266`: `sigguarded` pushes `current` onto `tss->front`
and pops it on the two normal exits. Neither the Windows backend (which never pushes) nor
the docs forbid the guarded function from using `setjmp`/`longjmp` for its own error
handling; a `longjmp` out of `guarded()` to a caller frame **above** `sigguarded` bypasses
both pop sites. The next `stdc_raise` on that thread walks `frame->guarded`/`decider`/`buf`
in freed stack — verified ASan `stack-use-after-scope` at `thrd_signal_handle_posix.c.ipp:292`;
a real signal delivered to that thread does the same *inside the handler*. On the
async-safe TLS path the stale frame is never cleared, so the corruption persists
indefinitely. Related sub-race (Y2b, code-level): a raise delivered after `guarded()`
returns but before `tss->front = old` executes still finds the frame and, if it chooses
`invoke_recovery`, silently discards `guarded()`'s return value — a few-instruction window
in the same family as V6 (below).

### 8.4 V6 [code-level, both backends, Medium] Frame published before `setjmp` completes -> longjmp into an uninitialised buffer

POSIX `sigguarded` (`thrd_signal_handle_posix.c.ipp:251-252`) and Windows `stdc_raise`
(`thrd_signal_handle_windows.c.ipp:270-271`) both execute `tss->front = &current` before
`setjmp(current.buf)` returns. A signal/exception delivered in that instruction window
runs `stdc_raise`/the vectored handler, which may `longjmp(current.buf)` before `setjmp`
has stored the environment — undefined behaviour. The window is a few instructions wide but
is exactly the async nature the API claims to handle.

### 8.5 W9 [C++ consumers, Low] longjmp across objects with non-trivial destructors is UB

`stdc_raise`'s `invoke_recovery` path (`thrd_signal_handle_posix.c.ipp:308`) and the
Windows vectored handler's `longjmp` (`thrd_signal_handle_windows.c.ipp:363`) skip C++
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
decider call (`thrd_signal_handle_posix.c.ipp:341,349`; Windows vectored handler
`thrd_signal_handle_windows.c.ipp:352,360`), and the matching decrements
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

- `siguninstall`'s `-1` failure path (`thrd_signal_handle_common.ipp.ipp:423-426`) leaks
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
- `config.h:122-130` opens and closes an empty `extern "C"` block — harmless but dead.
- `Readme.md:139` "Known bugs" section only lists the `pcpp` future work; none of the bugs
  above are listed.
- `doc/html/` is a committed Doxygen build output — version-controlled generated artifacts
  (churn, but not a bug).

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

`thrd_signal_handle_windows.c.ipp:282-293`: when `info != NULL` and room remains, the
function appends the `0xdeadbeefdeadbeef` marker and the raw context into
`info->ExceptionInformation[]` and bumps `NumberParameters` — mutating the caller's record
in place. If the caller passes a kernel-supplied `EXCEPTION_RECORD` (re-raising a genuine
fault from inside a filter — exactly the "pass on signal handling to this library" use
case documented in the header), the record the kernel will later inspect is altered. The
marker write is also racy if two threads re-raise through the same record.

### 9.4 W10 [code-level, Low] `signal_decider_destroy` double-destroy is an unguarded use-after-free

`thrd_signal_handle_common.ipp.ipp:612` (`free(p)`) unconditionally frees the handle. A
second `signal_decider_destroy` on the same pointer reads freed memory before the
double-free — same class as the `siguninstall` double-free (M1). No guard exists.

### 9.5 X5 [code-level, POSIX, Low] `stdc_raise` returns `true` even when the previous handler ignored the signal

`thrd_signal_handle_posix.c.ipp:364-368` — when the map has an entry for the signal but no
global decider claims it, `stdc_raise` calls `invoke_sigaction(&sa, ...)` and
unconditionally returns `true`. If the pre-library handler was `SIG_IGN` (or the default
action is ignore — SIGCHLD/SIGURG/SIGWINCH), `invoke_sigaction` returns `false` but
`stdc_raise` still returns `true`, violating the documented contract. Callers using the
documented `if(!stdc_raise(...)) { fall back }` idiom will not detect the silently-ignored
case.

### 9.6 X6 [code-level, Low] `siguninstall_system()` is a non-functional stub

`thrd_signal_handle_common.ipp.ipp:433-441` — the function only validates `version == 0`
and returns 0; it installs/removes nothing. The header documents it as "Uninstall a
previously system installed signal guard", but no system installation exists anywhere in
the codebase. An API that reports success for an operation it never performs is a latent
trap for future callers (and for the eventual C standard library integration this library
targets).

### 9.7 X11 [code-level, Low] `sigfence` with more than 8 arguments produces a confusing hard error

`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:85-95`) returns the 9th
argument as the count; `sigfence(a,...,i)` expands `WG14_SIGNALS_SIGFENCE_IMPL_i` — an
undefined identifier — yielding a cryptic compile error rather than a diagnostic about the
8-argument limit. (The 0-arg form works; verified.)

### 9.8 Z5 [code-level, Windows, Low] `siguninstall` clobbers an application-installed `SetUnhandledExceptionFilter`

`thrd_signal_handle_windows.c.ipp:423-424,437-438`: the library captures the
unhandled-exception filter present at first `siginstall` and restores exactly that filter
on full uninstall. If the application installs its own filter *after* the library's
`siginstall`, the library's `siguninstall` overwrites the application's filter with the
stale pre-library one. The unhandled-exception filter is a single process-global slot, and
ownership transfer on uninstall is asymmetric with the app's expectations. The POSIX
sibling is AA4: `uninstall_sighandler_impl`
(`thrd_signal_handle_posix.c.ipp:385-390`) restores `item->old_handler` — any
`sigaction()`/`signal()` call the application makes *after* `siginstall` is overwritten at
`siguninstall`, reverting the slot to the pre-library handler. The header's warning ("NOT
threadsafe with respect to other code modifying the global signal handlers") is framed as
a concurrency caveat and does not cover the sequential case.

### 9.9 Z7 [code-level, Low] `sigfillset_synchronous` / `_asynchronous_nondebug` / `_asynchronous_debug` crash on a NULL `set`

All three implementations do `memcpy(set, ..., sizeof(*set))` (POSIX) or the equivalent
(Windows) with no NULL check — a NULL argument is a NULL deref, unlike every other
argument-taking API in the library.

### 9.10 Z10 [code-level, Windows, Low, extends X9] `asynchronous_nondebug_sigset` silently omits most documented signals and includes the two `siginstall`-skipped ones

`thrd_signal_handle_windows.c.ipp:73-89` builds the nondebug set from only
`{SIGINT, SIGKILL, SIGSTOP, SIGTERM}`, whereas the header documents it as containing at
least SIGALRM, SIGCHLD, SIGCONT, SIGHUP, SIGINT, SIGKILL, SIGSTOP, SIGTERM, SIGTSTP,
SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGPOLL, SIGPROF, SIGURG, SIGVTALRM. Additionally
`SIGKILL` and `SIGSTOP` (which `siginstall` deliberately skips) are in the set, so
`siginstall(sigfillset_asynchronous_nondebug())` claims two signals that are never
installed. The debug set is the sibling defect (X9): `asynchronous_debug_sigset`
(`:97-104`) returns the *empty* set while the header documents "at least these POSIX
signals are within this set: SIGQUIT, SIGTRAP, SIGXCPU, SIGXFSZ" — a Windows consumer gets
a guard set that never matches anything.

### 9.11 Z11 [documentation, Low-Medium] the README's standalone `sigguarded` example is non-functional on every platform

`Readme.md:20-33` shows `sigguarded(...)` with no preceding `siginstall`. On POSIX the
frame stack is only consulted by the library's own `raw_signal_handler` (installed only by
`siginstall`) or by user `stdc_raise`; a genuine fault for an uninstalled signal runs the
kernel default (terminate) and never reaches the guard. On Windows, `sigguarded` alone
leaves the per-thread TSS uninitialised (X3). The README example should call
`siginstall`.

### 9.12 AA5 [code-level, both backends, Low] a *global* decider returning `sig_decision_invoke_recovery` has divergent, undocumented semantics

The enum documentation (`thrd_signal_handle.h:254-257`) says `sig_decision_invoke_recovery`
is "Thread local signal deciders only", yet global deciders share the same `sig_decide_t`
type and both backends accept it. **POSIX** (`thrd_signal_handle_posix.c.ipp:357-361`):
`if(res)` treats *any* non-zero decision as "claim and `return true`" — for
`invoke_recovery` the raise is claimed, no recovery is ever called, and for a genuine
fault the handler returns and the faulting instruction re-executes (an infinite re-fault
livelock),
**even when a guarding `sigguarded` frame exists**. **Windows**
(`thrd_signal_handle_windows.c.ipp:354-366`): the same value causes a
`longjmp(tss->front->buf, 1)` into the top frame when one exists (or
`EXCEPTION_CONTINUE_EXECUTION` / NULL-deref otherwise). So one enum value produces "claim,
no recovery, re-fault" on POSIX and "unwind to top frame" on Windows. Neither backend
documents or diagnoses this for global deciders.

### 9.13 AA6 [code-level, Windows, Low] user `EXCEPTION_RECORD` parameters masquerade as `rsi->addr` / `rsi->error_code`

`thrd_signal_handle_windows.c.ipp:186-191` reads `ExceptionInformation[1]` and
`ExceptionInformation[2]` as `addr` and `error_code` with no `NumberParameters` check. For
a user raise via `stdc_raise(signo, info, ctx)` the array holds the *caller's own
parameters*, so deciders see arbitrary user data in `addr`/`error_code` (deterministic for
non-NULL `info`, not garbage). Any decider keying on the NTSTATUS in `error_code` gets
different values for user raises than for genuine faults.

### 9.14 AA7 [code-level, C++ conformance, Low] `calloc` allocates C++ objects containing `std::atomic_uint` members without starting their lifetime

`tss_async_signal_safe.c.ipp:93-109` (`tss_async_signal_safe_create`) and `:203-204`
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
| V1 | Critical | Installed package: no headers installed; `find_package` fails (PACKAGE_INIT never expanded) | `CMakeLists.txt:50-70`, `cmake/ProjectConfig.cmake.in` |
| 2.1 | High | tss deinit count/state race -> UAF **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:136-168` |
| 2.2 | High | `siguninstall` vs in-flight `stdc_raise` -> container UAF (also Windows, W4) **[FIXED 2026-08-10]** | `thrd_signal_handle_posix.c.ipp:314-368` |
| W1 | High | dangling map entry after `deinit_state` OOM; next get/init returns freed pointer **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:201-213` |
| W2 | High | deciders get indeterminate/stale `error_code`/`addr`/`raw_info` for `stdc_raise(signo,NULL,NULL)` (confirmed) | `thrd_signal_handle_posix.c.ipp:186-199,327` |
| W4 | High | 2.2 container UAF also in Windows vectored handler; filter removable mid-raise **[FIXED 2026-08-10]** | `thrd_signal_handle_windows.c.ipp:329-368` |
| W5 | High | Windows `stdc_raise` never returns false; unclaimed raises kill the process via WER | `thrd_signal_handle_windows.c.ipp:252-300` |
| X1 | High | `thread_init` UAF on `deinit_state` after all registered threads exited (confirmed) **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:162-166,201-214` |
| X2 | High | `destroy` UAF on `deinit_state` after all registered threads exited (confirmed) **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:117-121` |
| Y1 | High | `install_sighandler` lock leak when `install_sighandler_impl` fails (confirmed) | `thrd_signal_handle_common.ipp.ipp:305-311` |
| Z3 | High | `sigguarded`/`stdc_raise` after full `siguninstall` = heap-UAF on destroyed TSS (confirmed; extends 2.4/V7) **[FIXED 2026-08-10]** | `thrd_signal_handle_common.ipp.ipp:276-281`, `tss_async_signal_safe.c.ipp:177` |
| V2 | High | Windows vectored handler NULL-derefs `tss->front` on fresh threads | `thrd_signal_handle_windows.c.ipp:357-361` |
| V3 | High | Windows `sigismember(guarded, 0)` UB; C++ exceptions swallowed by `sigguarded` | `thrd_signal_handle_windows.c.ipp:200`, `thrd_signal_handle.h:52-63` |
| V4 | High | Windows `stdc_raise` aborts for all unsupported signos | `thrd_signal_handle_windows.c.ipp:112-134` |
| Z1 | Med-High | verstable `signo_to_sighandler_map_t` never initialised -> NULL-metadata crash (NSIG >= 1024) | `thrd_signal_handle_common.ipp.ipp:58-135,174-179` |
| X3 | Med-High | Windows `sigguarded` never inits per-thread TSS -> NULL-deref in vectored handler on fresh threads | `thrd_signal_handle_windows.c.ipp:219-249` |
| AA1 | Med-High | `signal_decider_destroy` NULL-deref/UAF after `siguninstall`->`siginstall` orphans the decider node (confirmed) | `thrd_signal_handle_common.ipp.ipp:328-364,443-614` |
| 2.3 | Med | `sighandlers_count` increment before TSS-create check **[FIXED 2026-08-10]** | `thrd_signal_handle_common.ipp.ipp:316-323` |
| 2.4 | Med | Dead code after `return` in `sig_global_tss_state_destroy` **[FIXED 2026-08-10]** | `thrd_signal_handle_common.ipp.ipp:276-281` |
| 2.5 | Med | `thread_init` returns success for NULL item | `tss_async_signal_safe.c.ipp:186-190` |
| 2.6 | Med | `tss_async_signal_safe_*` NULL handle crash (also Z8, X8) | `tss_async_signal_safe.c.ipp:93-243` |
| 3.1 | Med | Spinlock not async-signal-safe | `lock_unlock.h` |
| 3.3 | Med | `SA_NOCLDWAIT`/`SA_NODEFER`/no `SA_RESTART` semantics | `thrd_signal_handle_posix.c.ipp:371-383` |
| 4.1 | Med | FreeBSD-only fallback for `get_current_thread_id` | `current_thread_id.c.ipp:70-72` |
| 8.1 | Low | Post-longjmp access to modified non-volatile locals | `thrd_signal_handle_posix.c.ipp:252-258` |
| V5 | Med | Windows global deciders run twice per exception (once under a debugger) | `thrd_signal_handle_windows.c.ipp:408-427` |
| V6 | Med | setjmp-buffer race: frame published before setjmp completes | `thrd_signal_handle_posix.c.ipp:251-252`, `thrd_signal_handle_windows.c.ipp:270-271` |
| V7 | Med | `siguninstall` during another thread's `sigguarded` frees live TSS (fallback path) | `thrd_signal_handle_common.ipp.ipp:348-360` |
| W3 | Med | nested delivery overwrites the frame `rsi` mid-decider (SA_NODEFER re-entrancy) | `thrd_signal_handle_posix.c.ipp:294-295` |
| W6 | Med | `PROJECT_IS_TOP_LEVEL` needs CMake >= 3.21 vs `minimum_required(3.15)` — tests silently skipped | `CMakeLists.txt:1,72` |
| X4 | Med | Windows `EXCEPTION_STACK_OVERFLOW` unmapped (POSIX handles it as SIGSEGV) | `thrd_signal_handle_windows.c.ipp:135-164` |
| Y2 | Med | user `longjmp` out of `guarded()` -> dangling `tss->front` -> stack UAF in `stdc_raise` (confirmed) | `thrd_signal_handle_posix.c.ipp:251-266,292` |
| 8.6 | Med | never-returning global decider (e.g. `siglongjmp`) leaks node + container refcounts forever | `thrd_signal_handle_posix.c.ipp:341,349`, `thrd_signal_handle_windows.c.ipp:352,360` |
| Z2 | Low-Med | `stdc_raise(signo,NULL,NULL)` hands NULL `siginfo_t*`/`ucontext_t*` to pre-existing SA_SIGINFO handler (confirmed) | `thrd_signal_handle_posix.c.ipp:314-368` |
| X10 | Med-Low | musl builds fail to compile (`struct __siginfo` fallback) | `thrd_signal_handle.h:207` |
| W7 | Low | MSVC library build lacks `/WX` | `CMakeLists.txt:44-48` |
| 5.2 | Low | static lib requires an undeclared C++ runtime on non-`__cxa_thread_atexit()` platforms | `CMakeLists.txt:71-75`, `cmake/ProjectConfig.cmake.in` |
| Y7 | Low | `project()` unconditionally requires CXX; C-only toolchains cannot configure | `CMakeLists.txt:11` |
| W8 | Low | `stdc_raise` mutates caller's `EXCEPTION_RECORD` in place | `thrd_signal_handle_windows.c.ipp:282-293` |
| W9 | Low | longjmp skips C++ destructors in guarded frames (UB; 4611 disabled) | `thrd_signal_handle_posix.c.ipp:308`, `thrd_signal_handle_windows.c.ipp:363` |
| W10 | Low | `signal_decider_destroy` double-destroy = unguarded UAF | `thrd_signal_handle_common.ipp.ipp:612` |
| X5 | Low | `stdc_raise` returns true when the previous handler ignored the signal | `thrd_signal_handle_posix.c.ipp:364-368` |
| X6 | Low | `siguninstall_system` is a no-op stub that reports success | `thrd_signal_handle_common.ipp.ipp:433-441` |
| X7 | Low | out-of-range `signo` -> `sigismember` shift UB on macOS/BSD (glibc safe) | `thrd_signal_handle_posix.c.ipp:292` |
| X8 | Low | `tss_async_signal_safe_create` validates neither argument | `tss_async_signal_safe.c.ipp:93-109` |
| X9 | Low | Windows `asynchronous_debug_sigset` returns empty set, contradicts docs | `thrd_signal_handle_windows.c.ipp:97-104` |
| X11 | Low | `sigfence` with >8 args -> cryptic error | `thrd_signal_handle.h:85-95` |
| X12 | Low | `thrd_join` error check is wrong (`ret != -1`); `thrd_create` unchecked calloc | `test/test_common.h:43-59` |
| Y3 | Low | `tss_async_signal_safe_destroy` frees `mem` with lock held **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:111-134` |
| Y4 | Low | deinit `attr.destroy` failure leaves stale TID entry + count never decremented (extends 3.6) **[FIXED 2026-08-10]** | `tss_async_signal_safe.c.ipp:150-158` |
| Y5 | Low | no `pthread_atfork`; stale TID caches/map across `fork()` | `current_thread_id.c.ipp:50-56,78-84`, `tss_async_signal_safe.c.ipp:81-91` |
| Y6 | Low | missing `NSIG` -> zero-length array + silently no-op `siginstall` | `thrd_signal_handle_common.ipp.ipp:58-62,381` |
| Y9 | Low | forced `HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles silently (refines 4.2) | `config.h:41-51,53-67` |
| Z4 | Low | negative `signo` -> UB frame-walk shift / Windows `abort()` (extends X7/V4) | `thrd_signal_handle_posix.c.ipp:292`, `thrd_signal_handle_windows.c.ipp:112-134` |
| Z5 | Low | Windows `siguninstall` clobbers an app filter installed after `siginstall` | `thrd_signal_handle_windows.c.ipp:437-438` |
| Z6 | Low | `siginstall(NULL)` on glibc installs over `SIGCANCEL`/`SIGSETXID` (32/33) and realtime 34-64 | `thrd_signal_handle_posix.c.ipp:371-383,143-184` |
| Z7 | Low | `sigfillset_*` NULL `set` -> `memcpy` NULL-deref | both backends |
| Z8 | Low | `tss_async_signal_safe` NULL/double destroy + post-destroy `get`/`thread_init` unguarded | `tss_async_signal_safe.c.ipp:111-134,226-243` |
| Z9 | Low | `thread_init`'s unlocked `attr.create` breaks THREADSAFE claim for concurrent first-use | `tss_async_signal_safe.c.ipp:184-191` |
| Z10 | Low | Windows nondebug set omits documented signals, includes `SIGKILL`/`SIGSTOP` (extends X9) | `thrd_signal_handle_windows.c.ipp:73-89` |
| Z11 | Doc | README standalone `sigguarded` example non-functional on all platforms | `Readme.md:20-33` |
| AA2 | Low | filc `-DDISABLE_INLINE_ASM=1` is a no-op for the library; toolchain paths hardcoded | `thrd_signal_handle.h:97-154`, `cmake/filc-toolchain.cmake` |
| AA3 | Low | `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` breaks the test build (confirmed) | `thrd_signal_handle.h:77`, `test/thrd_sigfpe_test.c:64` |
| AA4 | Low | `siguninstall` (POSIX) discards post-`siginstall` app handler changes (Z5 sibling) | `thrd_signal_handle_posix.c.ipp:385-390` |
| AA5 | Low | global decider `invoke_recovery`: POSIX claims-without-recovery vs Windows unwinds-to-frame | `thrd_signal_handle_posix.c.ipp:357-361`, `thrd_signal_handle_windows.c.ipp:354-366` |
| AA6 | Low | Windows user `EXCEPTION_RECORD` params leak into `rsi->addr`/`error_code` | `thrd_signal_handle_windows.c.ipp:186-191` |
| AA7 | Low | C++ object-lifetime UB: `calloc` for `std::atomic_uint` members | `tss_async_signal_safe.c.ipp:93-109,203-204` |
| AA8 | Low | `my_current_thread_id` cache uses plain `_Thread_local`, not async-safe TLS | `tss_async_signal_safe.c.ipp:81-91` |
| AA9 | Low | `stdc_raise(SIGFPE)` code != real `INT_DIVIDE_BY_ZERO` code; `SIGBUS`->`IN_PAGE_ERROR` | `thrd_signal_handle_windows.c.ipp:112-134` |

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
