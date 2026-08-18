# Exhaustive implementation analysis: wg14_signals

Scope: every header, every source file, both backends (POSIX/Windows), the header-only
configuration, the fallback hash-table TLS path and the async-signal-safe TLS path, all
error paths, and all build configurations exercised and *not* exercised by CI. Reviewed
against the vendored N3924 rev 4 wording (`docs/proposed-wording.md`). Current HEAD:
`1eea326`.

Findings are listed in priority order, the fixable ones first and the adjudicated
wontfix ones last (see their headings); the ranking criteria are explained
in §4 "Priority ordering rationale". Each finding's identity is its unique four-letter
code, shown in its heading (e.g. `SPIN`), and all citations in this file, in
`plans/ideas.md`, and in the source comments use the codes, so findings can be added,
moved or reordered without rewriting citations. Items
marked **[confirmed]** were reproduced on macOS (arm64, clang 17, ASan/UBSan where
noted); items marked **[probe-verified]** were verified against the current tree with
throwaway probes (scratch area, temporary artifacts, not part of the tree); items marked
**[wontfix]** were adjudicated as not-to-fix, with the rationale recorded in their
heading body. Windows-only
items are code-level findings (no Windows host available) but were verified against the
MSVC build matrix in CI. Where merged findings touch the same machinery (e.g. `DEIN`
extends `UNTL`), the earlier finding is
referenced rather than duplicated. Removed findings: the `JLGS` dangling-frame defect (fixed cooperative case,
wontfix uncooperative case, 2026-08-17), the `SJMP` setjmp-buffer race (fixed by the
publish-after-`setjmp` change in the working tree, 2026-08-17), and the `NSTR` nested-
delivery `rsi` race (fixed 2026-08-17) are no longer listed. Also removed on 2026-08-18 as
fixed/done: `NDEC` (never-returning decider pinning the per-raise
`stdc_raise_initiated_exception` chain and `tss->front`; both consequences fixed in the
working tree), `SDDF` (out-of-lock `free` of decider nodes; the in-lock fix is applied and
the residual is safe by construction), `PFXM` (the `WG14_SIGNALS_PREFIX` misuse claim
was refuted and the test spellings fixed), and `VDED` (the Windows V5 dedup cache is now
invalidated on every dispatch: the unhandled filter clears the entry before each pass and
the vectored continue handler consumes it when reusing the recorded decision, so a later
exception reusing a record's stack address can never skip the global-decider pass),
and `GLIN` (the `NSIG >= 1024` verstable-map branch: `sig_global_state()` now
initialises the `signo_to_sighandler_map` exactly once behind an atomic single-writer
gate, so two threads' first concurrent calls cannot race on the table fields), and
`DFLT` (the POSIX `invoke_sigaction` SIG_DFL default-action re-raise now saves and
restores the library's installed handler, so stop/continue defaults no longer leave the
kernel handler reset to `SIG_DFL` after the process resumes from a stop).

---

## 1. Findings, in priority order

### `UNKN` raw_signal_handler on unknown signals silently installs SIG_DFL and re-raises

`thrd_signal_handle_posix.c.ipp:223-239`: if `stdc_raise` returns false, the handler
replaces itself with `SIG_DFL` and invokes `invoke_sigaction(&sa, ...)` where `sa` is the
freshly-minted SIG_DFL struct — for a default-ignore signal it returns false (no re-raise,
signal silently dropped); for others it re-raises as default. Reasonable, but the comment
admits "It shouldn't happen that this handler gets called when we have no knowledge of
the signal".

### `TIDR` Thread-ID reuse with stale hash-table entries

`tss_async_signal_safe` maps are keyed by kernel thread ID (`current_thread_id`). If a
thread exits without running its atexit deinit (abnormal termination, `_exit` within a
thread is process-wide, cancellation corner cases, or `thread_atexit` registration
failing), the map retains the entry under that TID. A later thread that reuses the same
TID will observe the *previous* thread's value (never its own), and destruction may run
with stale state. There is no TID-generation counter.

### `SDCF` signal_decider_create failure path partially self-destroys correctly but leaves warning-path signals uncounted

When `calloc` fails mid-loop, `signal_decider_create` calls `signal_decider_destroy(ret)`
on the partially-built handle, but `signal_decider_destroy` returns -1 (errno unchanged)
when it finds no matching slots — a misleading error signal. Additionally,
`WG14_SIGNALS_STDERR_PRINTF` runs while `state->lock` is held, which is slow and can
itself trigger a signal while the lock is held (see `SPIN`).

### `UCLK` [code-level, both backends, Low] `tss_async_signal_safe_destroy` runs the user's `attr.destroy` callback with `mem->lock` held

`tss_async_signal_safe.c.ipp:122-146` — destroy acquires `mem->lock` and invokes
`mem->attr.destroy(it.data->val)` for every registered thread while the lock is held
(and while the map entries are still live). A re-entrant library call from the callback —
e.g. `tss_async_signal_safe_get()` on the same handle, which the public header documents
THREADSAFE ASYNC-SIGNAL-SAFE (`tss_async_signal_safe.h:68-72`) — self-deadlocks on the
non-recursive spinlock, exactly the `SPIN` mechanism but triggered by a documented-valid
callback pattern instead of a signal handler. This is the mirror of `UCRE` (whose `create`
callback deliberately runs unlocked): `create` trades re-entrancy for cross-thread
serialisation of the user callback, while `destroy` serialises it under the lock with no
documented warning. Additionally the fallback path's `sig_global_tss_state_destroy` runs
inside the library's *global* `state->lock` (`thrd_signal_handle_common.ipp.ipp:496-499`),
so the last `siguninstall`'s per-thread value destruction executes user-visible callbacks
under the global lock (its attr callbacks are library-internal `free`s today, so this is
only a latent hazard if the sig TSS attr is ever customised). Fix direction: either
document the re-entrancy constraint, or gather the values under the lock and invoke the
callbacks after unlocking (with the destroy-vs-deinit lifetime rules of `DEIN` kept in
mind).

### `NSIH` [confirmed, Low-Med] `stdc_raise(signo, NULL, NULL)` hands off to a pre-existing `SA_SIGINFO` handler with NULL `siginfo_t *` and NULL `ucontext_t *`

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

### `IGND` [code-level, POSIX, Low] `stdc_raise` returns `true` even when the previous handler ignored the signal

`thrd_signal_handle_posix.c.ipp:394-397` — when the map has an entry for the signal but no
global decider claims it, `stdc_raise` calls `invoke_sigaction(&sa, ...)` and
unconditionally returns `true`. If the pre-library handler was `SIG_IGN` (or the default
action is ignore — SIGCHLD/SIGURG/SIGWINCH), `invoke_sigaction` returns `false` but
`stdc_raise` still returns `true`, violating the documented contract. Callers using the
documented `if(!stdc_raise(...)) { fall back }` idiom will not detect the silently-ignored
case. **Probe-verified (macOS arm64):** `stdc_raise(SIGUSR1, NULL, NULL)` with
SIGUSR1 installed and a pre-install `SIG_IGN` disposition returned `true` (the signal was
silently ignored); with a benign pre-install handler it returned `true` and the previous
handler ran once.

### `ZERO` [semantics, both backends, Low] `stdc_raise` returns `true` when zero deciders were called **[probe-verified]**

N3924 (7.14.3.2): "returns true if at least one signal decider installed under this
facility was called." With a map entry for the signal but an empty decider list, both
backends hand off to the previously installed handler and return `true`:
`thrd_signal_handle_posix.c.ipp:404-409` (`invoke_sigaction` then unconditional
`return true`) and `thrd_signal_handle_windows.c.ipp:535-541` (`CONTINUE_SEARCH`;
`stdc_raise_initiated_exception_was_unclaimed` stays 0 so `stdc_raise` returns `true` at
`thrd_signal_handle_windows.c.ipp:409`). On Windows the same deviation occurs when the
raise is caught by the *user's own* `__try/__except` frame (no library decider involved,
`unclaimed == 0`). Probe 1 (`stdc_raise(SIGUSR1, NULL, NULL)` with SIGUSR1 installed and
zero deciders, previous handler installed via `signal()`): returned `true`, old handler
called. The proposal's criterion is "decider called", not "decider claimed", so the case
where deciders were called but all returned `next_decider` (then the hand-off runs and the
implementation returns `true`) *is* conforming; only the zero-decider case deviates.
Distinct from finding `IGND` (which is the SIG_IGN hand-off vs the *header's own* "false when
no decider claims" contract): `IGND`'s scenario is conforming to the proposal when a decider
was called, and `ZERO`'s scenario is a proposal deviation the header does not mention.

### `WFRM` [code-level, Windows, Med] global deciders run *before* thread-local frame deciders on Windows (N3924 7.14.1 requires thread-local first); frame deciders are skipped entirely for software raises of uninstalled signals

N3924 7.14.1: "The ordered sequence begins with thread-locally installed signal
deciders ... followed by signal deciders installed globally". On Windows the dispatch
order is: vectored exception handlers (the library's `win32_vectored_exception_function`,
`thrd_signal_handle_windows.c.ipp:431-542`, which runs the *global* decider pass)
-> frame-based filters (`win32_exception_filter`, `:257-283`, which runs the
*thread-local* frame decider) -> unhandled filter -> vectored continue handler. The
vectored handler's precedence inverts the wording's order for every raise that has
both a guarding frame and a global decider:

1. A global decider that claims (`sig_decision_resume_execution`) an exception inside
   a guarded frame returns `EXCEPTION_CONTINUE_EXECUTION` and the frame decider never
   runs — for a genuine fault this re-executes the faulting instruction (re-fault
   livelock) where POSIX would have let the frame decider decide first (possibly
   `invoke_recovery` + `longjmp`); for a software raise the claim is resolved before
   the frame is consulted at all.
2. A software raise (`stdc_raise`) of an *uninstalled* signal inside a guarding frame:
   the vectored no-map-entry branch returns `EXCEPTION_CONTINUE_EXECUTION`
   (`:466-473`) *before* the frame filter can run, so the thread-local frame decider
   is never invoked and `stdc_raise` returns false — while POSIX walks the frames
   first and invokes the frame decider for the same call
   (`thrd_signal_handle_posix.c.ipp:322-347`), which may even recover via
   `sig_decision_invoke_recovery`. A frame that guards a signal thus protects the
   raise on POSIX but not on Windows when the signal is not installed globally.

GDIR covers what `sig_decision_invoke_recovery` *means* for a global decider; this
finding is the separate dispatch-precedence inversion, and it is a silent
cross-platform behavioural divergence in the core raise machinery (the same source
line `stdc_raise(...)` inside `sigguarded(...)` behaves differently per backend).
Fix direction: have the vectored function consult the per-thread frame stack
(`tss->front`) before running the global pass (the frame deciders themselves must
still run in the filter to be able to use `EXCEPTION_EXECUTE_HANDLER`, so this needs
the filter/vectored split rethought), or document the inversion.

### `PRCR` [semantics, both backends, Low] `signal_decider_create` before `siginstall` silently loses the decider **[probe-verified]**

N3924 models deciders as installed globally and invoked whenever "siginstall has been
called in the current program execution" — order between `signal_decider_create` and
`siginstall` is not constrained. The implementation registers each decider node into the
per-signal container that exists *at create time*: a signal with no installed handler gets
a NULL slot plus a stderr warning (`thrd_signal_handle_common.ipp.ipp:672-683`), and a
later `siginstall` builds a fresh container (`install_sighandler`, `:405-463`) that the
decider was never linked into. Result: the decider never runs for that signal, the handle
reports success, and only the stderr warning hints at the loss. Probe 3 (create decider
for SIGUSR1, then `siginstall`, then raise): "decider ran 0 times, raise returned true"
(and the warning was printed). The reverse order (install, then create) works. The
ordering constraint is nowhere documented; either document it or re-link deciders into
newly created containers.

### `AMBI` sigguarded failure return is indistinguishable from a legitimate -1

`sigguarded` failure returns `ret.int_value = -1`, which is indistinguishable from a
guarded function legitimately returning -1; no error return is documented for `sigguarded`.

### `ORDR` [semantics, both backends, Med] global-decider iteration order is the reverse of the N3924 7.14.1 ordering clauses **[probe-verified]**

N3924 (7.14.1, global deciders): "The deciders are called in order of: those with
`callfirst == true` most recently installed last, and then those with `callfirst == false`
in order of most recently installed first." The implementation:
`signal_decider_create` inserts callfirst nodes at the list front and tail nodes at the
back (`thrd_signal_handle_common.ipp.ipp:698-707`), and both `stdc_raise` paths walk the
list front-to-back (`thrd_signal_handle_posix.c.ipp:373-402`,
`thrd_signal_handle_windows.c.ipp:485-533`). So the callfirst group runs *newest first*
and the tail group runs *oldest first* — the exact inverse of the wording's clause in
both groups. Probe 2 (create A callfirst, B tail, C tail, D callfirst; raise): call order
`D A B C` (then the previous handler), i.e. callfirst newest-first, tail oldest-first.
Same reversed phrasing appears for the thread-local sequence ("in order of most recently
installed last for that thread"): the implementation's frame walk is newest-first
(`thrd_signal_handle_posix.c.ipp:322-347`), which is the only sane stacking order (the
innermost guard must see the raise first).

Important nuance — the wording is internally contradictory, and the implementation follows
the *other* clause: `signal_decider_create`'s own description says callfirst "installs the
function at the top of the list to be called before any other functions currently in the
list", and "otherwise it is installed at the end of the list" — which is exactly the
implementation's behavior. The 7.14.1 Introduction ordering clauses describe the opposite
iteration in all three places. Either the Introduction clauses are a systematic wording
bug (most likely: "most recently installed last" should read "first", and vice versa), or
the reference implementation must reverse all three iteration orders. A WG14 wording
clarification is required before this can be fixed without breaking either the create()
descriptions or the stacking intuition; flag for the wording group.

**Note (2026-08-18):** adjudicated — the proposed wording (N3924 7.14.1 Introduction) is
wrong and should be fixed, not the implementation: "most recently installed last" should
read "first" (and vice versa) in the three ordering clauses, in `docs/proposed-wording.md`.
The reference implementation follows `signal_decider_create`'s own description (callfirst
at the top / tail at the end) and the thread-local stacking intuition, and will not be
reversed. `ORDR` stays open only as the record of the required wording fix.

### `ENUM` [public API, both backends, Low] enum member is named `sig_decision_invoke_recovery`, the N3924 wording names it `sig_decision_call_recovery`

`thrd_signal_handle.h:419-429` declares the third member of `sig_decision_t` as
`WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery)`. The N3924 wording's required member
list is `sig_decision_next_decider`, `sig_decision_resume_execution`,
`sig_decision_call_recovery` (7.14.1 "The `sig_decision_t` enumeration shall contain at
least the following members..."). The name `sig_decision_call_recovery` appears nowhere in
the tree; `sig_decision_invoke_recovery` is used by the backends
(`thrd_signal_handle_posix.c.ipp:334`, `thrd_signal_handle_windows.c.ipp:273`), seven
tests, the README, and the generated Doxygen. The N3924 revision history
("Renamed `thrd_signal_invoke` to `sigguarded`" etc.) does not mention this member being
renamed, so the implementation kept a pre-N3924 name. Consequence: code written against
the proposal wording (`return sig_decision_call_recovery;`) fails to compile against this
reference implementation — the one public identifier that a consumer's decider must
literally spell.

### `TYDF` [public API, both backends, Low] typedef is named `thrd_raised_signal_error_code_t`, the N3924 wording names it `stdc_siginfo_error_code_t`

`thrd_signal_handle.h:351-355` defines `thrd_raised_signal_error_code_t` (the only
remaining `thrd_*` public identifier; the struct itself was renamed to `stdc_siginfo`).
The N3924 wording requires "`stdc_siginfo_error_code_t` which is an implementation-defined
complete native error code type" (7.14.1), matching the revision history's blanket
"Renamed `thrd_raised_signal_info*` to `stdc_siginfo*`". Used at
`thrd_signal_handle.h:393` (`stdc_siginfo::error_code`) and `thrd_signal_handle_windows.c.ipp:248`;
no occurrence of `stdc_siginfo_error_code_t` exists. Same consequence as `ENUM`: consumer code
using the proposal's type name fails to compile. Both `ENUM` and this renaming are mechanical renames;
the enum-member rename additionally needs a README + doc + test sweep.

### `VSDT` [public API, Windows, Low] Windows `sigemptyset`/`sigfillset`/`sigaddset`/`sigdelset` return `void`, the N3924 synopses return `int` and "always return zero"

`thrd_signal_handle.h:44-75` defines the four helpers as `static inline void` on `_WIN32`
(`sigismember` returns `bool`). The N3924 synopses (7.14.2.1/7.14.2.2) declare
`int sigemptyset(sigset_t *setp)`, `int sigfillset(...)`, `int sigaddset(...)`,
`int sigdelset(...)` with "This function always returns zero". Code written against the
proposal, e.g. `if(sigemptyset(&ss) != 0)`, compiles on POSIX (the platform's `int`
functions are used there) and fails to compile on Windows (comparison of `void`). Two
further consequences:

1. On POSIX the implementation forwards to the host libc's sigset functions, which may
   return `-1`/`EINVAL` for an out-of-range `signo` (e.g. `sigaddset(&ss, 64)` on glibc)
   — the proposal's "always returns zero" contract is not guaranteed by the reference
   implementation on POSIX either (the platform contract is "0 on success, -1 on error").
   As a standard-library drop-in (the stated goal of the project) this needs wrapping or
   a documented divergence.
2. `sigismember`'s `bool` return satisfies "positive one is returned... zero" only by
   accident of `bool` layout; it is fine in practice.

### `TLSD` WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL detection is too optimistic; forcing it on Apple is silently unsafe

`config.h:51-61` enables async-safe TLS for *any* `__GNUC__` (which includes clang) on
any non-Apple platform. This is only true where the toolchain actually supports
`tls_model("initial-exec")` and the libc reserves static TLS for dlopened libraries.
Also, a user-defined `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on a compiler that is
neither `__GNUC__` nor `_MSC_VER` leaves `WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL` undefined
while code references it -> compile error with no diagnostic. **Verified probe:** forcing
`HAVE_ASYNC_SAFE_THREAD_LOCAL=1` on Apple compiles and links — clang on macOS *accepts*
`__attribute__((tls_model("initial-exec")))` on Mach-O (parsed and ignored/approximated) —
but the real consequence is silent: Mach-O TLS accesses (`tlv_get_addr` machinery) are not
async-signal-safe, so the library quietly violates its own "ASYNC SIGNAL SAFE" contract,
including inside `current_thread_id()`/`tss_async_signal_safe_get()` called from handlers.
A silent safety regression, not a diagnostic.

### `PTHD` [confirmed, fallback path, Med] the pthread-key `thread_atexit` fallback drops *every* registered callback on Darwin: pthread key destructors run after the Mach-O TLS block has been torn down, so the destructor reads a freshly-initialised (empty) `thread_atexit_items` list

`thread_atexit.c.ipp:158-233` (the POSIX `pthread_key` fallback, used whenever
`WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` is undefined and the TU is C — i.e. any
header-only C consumer that does not define the macro) stores the per-thread callback
list in a `_Thread_local` (`thread_atexit_items`, `:167-168`) and drains it from the
pthread key destructor (`thread_atexit_run`, `:171-188`). **Probe-verified on macOS 15.7.9
arm64 (clang 17):** at key-destructor time on Darwin the exiting thread's TLS block has
already been torn down and any TLS access re-creates it with initialiser values, so the
destructor runs but reads `thread_atexit_items == NULL` — every registered callback is
silently dropped (the destructor's *argument* is preserved, only the TLS read is zeroed).
Consequences on this path:

1. `tss_async_signal_safe_thread_init`'s `thread_atexit(func, mem->state)`
   (`tss_async_signal_safe.c.ipp:264-266`) never runs `tss_async_signal_safe_thread_deinit`:
   the per-thread map entry, the TID key and the shared `deinit_state` leak at every
   thread exit, and `TIDR`'s stale-entry trigger becomes the *normal* case.
2. `sig_global_tss_state_init`'s `thread_atexit(free, mem)`
   (`thrd_signal_handle_common.ipp.ipp:296-317`) never frees the per-thread raise state.

CI never compiles the branch: every CI platform's probe finds `__cxa_thread_atexit` (and
the library build prefers `thread_atexit.cpp` when `__cxa` is absent), so only header-only
C consumers without the define, or exotic non-`__cxa` POSIX platforms, hit it. Fix: store
the list head in the key value itself (the destructor's argument survives) and/or probe
and reject the fallback on affected platforms at configure time.

### `MUSL` [code-level, Low-Med] `siginfo_t`/`ucontext_t` platform dispatch breaks musl and assumes non-POSIX spellings

`thrd_signal_handle.h:202-216` relies on `<signal.h>` defining `ucontext_t` (POSIX does
not require this; `<ucontext.h>` does) and dispatches the `siginfo_t` spelling by
platform: `_WIN32` / `__GLIBC__` / `__ANDROID__` / `else: struct __siginfo`. musl defines
`siginfo_t` as `struct siginfo`, not `struct __siginfo`, and defines neither `__GLIBC__`
nor `__ANDROID__` — so musl builds fall into the `struct __siginfo` branch and fail to
compile (the CI matrix only exercises glibc). The BSD/Android/glibc layout assumptions
are not portable.

### `SIGN` Missing `SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards

`thrd_signal_handle_posix.c.ipp:53-54` uses `SIGSYS`, and `:109` uses `SIGXCPU`/`SIGXFSZ`
without `#ifdef` guards (only `SIGPOLL` is guarded). On a POSIX platform that omits any of
these the file fails to compile. The library build now compiles with explicit feature-test
macros (plans/ideas.md §2), so glibc/musl consistently expose these
signals; the missing `#ifdef` guards remain for platforms that omit the signals entirely.

### `NEGS` [code-level, Low] out-of-range and negative `signo` in `stdc_raise` is UB on the POSIX frame walk

`thrd_signal_handle_posix.c.ipp:310-336` — the frame loop tests
`sigismember(frame->guarded, signo)` with no `signo < NSIG` check. On macOS/BSD
`sigismember` is a shift-count macro, so a negative `signo` is a negative shift count —
**probe-verified** to deterministically *alias another signal's frame decider*, not just
UB: `stdc_raise(-1)` inside a frame guarding SIGUSR2 invoked that frame's decider with
`rsi->signo == -1` (the masking turns `1u << -2` into `1u << 30`, i.e. signal 31; on
pre-Xcode-15 SDKs positive out-of-range signos wrap similarly, `stdc_raise(33)` matching a
frame guarding signal 1). On glibc `sigismember` returns 0 for out-of-range values, so the
bug is invisible there; on Windows both cases return false. Fix: bounds-check `signo`
before the walk.

### `RTIM` [code-level, Linux/glibc, Low] `siginstall(NULL)` installs handlers for glibc-internal signals 32/33 and realtime signals 34-64

On glibc `NSIG == 65`, so `siginstall(NULL)` (the pattern used by every test and the
README) installs the library's handler for signals 32 (`SIGCANCEL`) and 33 (`SIGSETXID`,
glibc's internal pthread-cancellation/setxid signals) and for all 31 realtime signals
34-64. `SIGCANCEL`/`SIGSETXID`: the library *replaces* glibc's internal handler and
chains to it on each delivery, adding latency to every pthread cancellation, allowing a
decider to swallow cancellation, and `SA_NODEFER` changes glibc's expected blocking
semantics. Realtime signals: the default path in `invoke_sigaction` resets to `SIG_DFL`
and re-raises, so a realtime-signal delivery routed through the library and then the
default re-raise terminates the process (the `DFLT` permanent-discard defect was fixed
in the working tree 2026-08-18 — the re-raise restores the installed handler — but a
terminating default still terminates), even though
`sigfillset_synchronous`/`_asynchronous_*` deliberately do not include realtime signals.
The internal and realtime ranges are neither skipped nor documented.

### `TOPL` [build, Med] `PROJECT_IS_TOP_LEVEL` requires CMake >= 3.21 while `cmake_minimum_required` is 3.15

`CMakeLists.txt:1` declares `cmake_minimum_required(VERSION 3.15 FATAL_ERROR)`, but line
231 gates the entire `test/` subdirectory on `PROJECT_IS_TOP_LEVEL`, a variable introduced
in CMake 3.21. On CMake 3.15-3.20 the variable is undefined and the condition silently
evaluates false: no tests, no `header_only_test` target, and `BUILD_TESTING` is ignored —
with no diagnostic. Either raise the minimum to 3.21 or use
`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`.

### `CPPR` Static library requires a C++ runtime, not declared [open on non-`__cxa_thread_atexit()` platforms]

On platforms without `__cxa_thread_atexit()` (e.g. Windows), `thread_atexit.cpp` is
compiled into the library and the CMake package does not express the C++ standard library
dependency, so a plain C consumer linking `libwg14_signals.a` there gets unresolved C++
runtime symbols. (On `__cxa_thread_atexit()` platforms the C file is compiled instead and
the library is all-C with no C++ runtime dependency.)

### `CSTD` CMake `CMAKE_C_STANDARD` cache variable is unused for consumers

The `CMAKE_C_STANDARD` cache variable set at `CMakeLists.txt:6` is not propagated to
consumers of the installed package; the `find_package` consumer must re-declare its own
`CMAKE_C_STANDARD` (the install consumer does, at `test/install_consumer/CMakeLists.txt:10-11`).

### `PCFG` ProjectConfig.cmake.in references non-existent export names

`cmake/ProjectConfig.cmake.in:6-11` conditionally includes
`@PROJECT_NAME@SlExports.cmake`/`@PROJECT_NAME@DlExports.cmake`, which are never
generated. Harmless (guarded by `EXISTS`), but misleading.

### `THRD` [test-harness, Low] `test_common.h` `thrd_join`/`thrd_create` defects

`test_common.h:81-100` — `thrd_join` checks `ret != -1`, but `pthread_join` returns an
error *number* (0 on success), never -1: on failure `*res` is left unset while the caller
proceeds as if the join succeeded. `thrd_create` (`:81-89`) dereferences the unchecked
`calloc` result (NULL deref on OOM). The benchmark and handle tests rely on this shim; the
harness masks real failures.

### `STOR` Test storage exhaustion

`test/async_signal_safe_tls_test.c:6-14` and `test/header_only_test.cpp:16-24`: `create`
does `*dest = storage_ptr++` on a 2-element array; any third `thread_init` (e.g., a
re-run of the main-thread init after the worker also inits, or the documented "safe to
call many times") writes out of bounds. **Extended:** the same applies to
`test/benchmark_async_signal_safe_tls_test.c:12-13`. The test also never verifies the
re-init and re-entrancy semantics documented in the API.

### `TCOV` No coverage of the failure/edge APIs

No test calls `siguninstall_system`, `sigfillset_synchronous/asynchronous_*` return
values, or `signal_decider_create` with NULL/empty guarded sets.

### `SABA` [code-level, Windows, Low] `stdc_raise(SIGABRT)` raises a non-continuable exception; "resume" from a decider loops

`SIGABRT` maps to `EXCEPTION_NONCONTINUABLE_EXCEPTION (0xC0000025)`. If any decider
returns "resume execution" for it, Windows re-raises `0xC0000025` (the OS cannot resume a
non-continuable exception), the vectored handler runs the decider again -> repeat loop.
POSIX has no such constraint for SIGABRT. (With no decider and an enclosing `__except` the
raise still works, which is why tests pass.)

### `MUTR` [code-level, Windows, Low] `stdc_raise` mutates the caller's `EXCEPTION_RECORD`

`thrd_signal_handle_windows.c.ipp:332-341`: when `info != NULL` and room remains, the
function appends the `0xdeadbeefdeadbeef` marker and the raw context into
`info->ExceptionInformation[]` and bumps `NumberParameters` — mutating the caller's record
in place. If the caller passes a kernel-supplied `EXCEPTION_RECORD` (re-raising a genuine
fault from inside a filter — exactly the "pass on signal handling to this library" use
case documented in the header), the record the kernel will later inspect is altered. The
marker write is also racy if two threads re-raise through the same record.

### `UFLT` [code-level, Windows, Low] `siguninstall` clobbers an application-installed `SetUnhandledExceptionFilter`

`thrd_signal_handle_windows.c.ipp:516-519`: the library captures the
unhandled-exception filter present at first `siginstall` and restores exactly that filter
on full uninstall. If the application installs its own filter *after* the library's
`siginstall`, the library's `siguninstall` overwrites the application's filter with the
stale pre-library one. The unhandled-exception filter is a single process-global slot, and
ownership transfer on uninstall is asymmetric with the app's expectations. The POSIX
sibling is `uninstall_sighandler_impl`
(`thrd_signal_handle_posix.c.ipp:414-419`) restores `item->old_handler` — any
`sigaction()`/`signal()` call the application makes *after* `siginstall` is overwritten at
`siguninstall`, reverting the slot to the pre-library handler. The header's warning ("NOT
threadsafe with respect to other code modifying the global signal handlers") is framed as
a concurrency caveat and does not cover the sequential case.

### `GDIR` [code-level, both backends, Low] a *global* decider returning `sig_decision_invoke_recovery` has divergent, undocumented semantics

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

### `MSQR` [code-level, Windows, Low] user `EXCEPTION_RECORD` parameters masquerade as `rsi->addr` / `rsi->error_code`

`thrd_signal_handle_windows.c.ipp:207-216` reads `ExceptionInformation[1]` and
`ExceptionInformation[2]` as `addr` and `error_code` with no `NumberParameters` check. For
a user raise via `stdc_raise(signo, info, ctx)` the array holds the *caller's own
parameters*, so deciders see arbitrary user data in `addr`/`error_code` (deterministic for
non-NULL `info`, not garbage). Any decider keying on the NTSTATUS in `error_code` gets
different values for user raises than for genuine faults.

### `CATM` [code-level, C++ conformance, Low] `calloc` allocates C++ objects containing `std::atomic_uint` members without starting their lifetime

`tss_async_signal_safe.c.ipp:96-112` (`tss_async_signal_safe_create`) and `:230-241`
(`deinit_state` allocation) use `calloc` for structs whose members include
`std::atomic_uint` (the `lock` and `count` fields). In C++ — the library is documented and
tested as C++-usable, and `thread_atexit` is compiled as C++ — no constructor runs for
those atomics, so using them is object-lifetime UB per the C++ standard (works on
MSVC/GCC/Clang because `std::atomic<unsigned>` is trivially default-constructible in
practice). The C path is unaffected (C11 `atomic_uint` is a plain type).

### `SFPD` [code-level, Windows, Low] `stdc_raise(SIGFPE)` raises a different exception code than a genuine integer divide-by-zero

`win32_exception_code_from_signal` maps SIGFPE -> `EXCEPTION_FLT_INVALID_OPERATION`
(0xC0000090), but the real hardware fault from `x / 0` on x64 is
`EXCEPTION_INT_DIVIDE_BY_ZERO` (0xC0000094); both reverse-map to SIGFPE. A decider that
inspects `rsi->error_code` (documented as "the NTSTATUS code") therefore observes different
codes for the same logical signal depending on whether it was user-raised or a real fault.
Similarly `stdc_raise(SIGBUS)` maps to `EXCEPTION_IN_PAGE_ERROR`, a semantically different
fault class from a real SIGBUS-equivalent. Cosmetic divergence, but the README/header
invite reading `error_code`.

### `DRGN` [code-level, Windows + C++-vector paths, Low] a callback registering a new `thread_atexit` callback during the thread-exit drain is silently dropped (MSVC TLS-directory path) or is UB (C++ vector path)

On the MSVC TLS-directory path (`thread_atexit.c.ipp:106-124`) the TLS callback
NULLs the list head and drains it exactly once at `DLL_THREAD_DETACH`; a callback
that runs `thread_init` on another handle during the drain (a user `attr.destroy`
callback calling `tss_async_signal_safe_thread_init` — a documented-valid pattern)
pushes a new item onto the list, but nothing ever re-runs the drain: the item, the
registered deinit and the second handle's `deinit_state` leak, and the second
handle's thread-exit cleanup never runs (same family as `PTHD`'s consequences, on
the Windows path). The C++ vector path (`thread_atexit.cpp.ipp:71-85`) is worse: a
registration during `std::vector` element destruction calls `emplace_back` on a
vector whose destructor is mid-flight — object-lifetime UB (works in practice on
MSVC/Clang). The pthread-key path is *not* affected: `thread_atexit` re-sets the key
value on re-registration, which re-triggers the destructor via
`PTHREAD_DESTRUCTOR_ITERATIONS` (re-registration re-sets the key value, re-triggering
the destructor). The
`__cxa_thread_atexit` path processes list entries in a loop and honours
append-during-drain. Low severity: needs a callback pattern and only leaks; flagged
because the "safe to call many times" `thread_init` contract invites exactly the
drain-time re-entry.

### `UCRE` [code-level, Low] `thread_init`'s unlocked `attr.create` breaks the THREADSAFE contract for concurrent first-use on distinct threads

`tss_async_signal_safe.c.ipp:211-218`: the user's `create` callback runs **without**
`mem->lock`, so two threads racing to first-initialise the same handle call the user's
`create` concurrently. The API documents `thread_init` as THREADSAFE and serialises the
map insert but not the create callback. The test suite's own `create` uses a shared
`static unsigned *storage_ptr`, so a two-worker concurrent first-use would be a data race
in the harness itself. Design note: either document that `create` must be thread-safe, or
serialise the create callback under the lock (at the cost of re-entrancy, cf. `REEN`).

### `TSSG` [docs/API contract, both backends, Low] `tss_async_signal_safe_destroy`'s documented precondition is stricter than the N3924 wording; `tss_async_signal_safe_get`'s header omits the wording's precondition

N3924 (7.30.6.6): destroy makes no requirement that the initialising threads have
exited. The implementation's header instead requires "All threads that initialised this
instance must have exited (e.g. been joined) before this is called"
(`tss_async_signal_safe.h:46-52`) and genuinely cannot destroy live threads' pointers
safely (the `DEIN` race) — a documented restriction the wording does not impose, and one
the library's own fallback path violates (`UNTL`). The double-destroy half was resolved
by the `TSSD` wontfix adjudication (2026-08-16); the `get` half below remains open.

`tss_async_signal_safe_get`: the wording requires `tss_async_signal_safe_thread_init()`
"shall have been called on the calling thread beforehand... otherwise the behavior is
undefined." The implementation returns NULL for an uninitialised thread
(`tss_async_signal_safe.c.ipp:273-290`) — a defined, benignly stronger behaviour — but the
header (`tss_async_signal_safe.h:68-72`) documents neither the precondition nor the NULL
fallback, so the "ASYNC-SIGNAL-SAFE" claim is asserted for a function whose first call per
thread performs the `my_current_thread_id()` cache fill (`MAST`) and a spinlocked map lookup
(`SPIN`) — see §2.

### `DECR` [docs, both backends, Low] `signal_decider_create`'s documentation describes a bool decider contract that no longer matches the enum `sig_decide_t`

`thrd_signal_handle.h:616-619` — "a decider function, which must return `true` if
execution is to resume, `false` if the next decider function should be called", and "A
user supplied value to set in the `raised_signal_info`". The decider type is
`enum sig_decision_t` (`sig_decision_next_decider` / `sig_decision_resume_execution` /
`sig_decision_invoke_recovery`), and the `raised_signal_info` identifier was renamed to
`stdc_siginfo` — both descriptions are stale. The bool text happens to be accidentally
consistent with the current enum layout (`false` == 0 == `next_decider`,
`true` == 1 == `resume_execution`, both on the POSIX frame switch and on the global
`if(res)` claim test), so a decider written from the docs compiles and behaves
sensibly — but a decider can never express `sig_decision_invoke_recovery` that way, and
the wording's own `sig_decision_t` semantics (7.14.1) are the contract a reference
implementation should document. Pure documentation hygiene; flagged because the stale
contract text is the first thing a consumer of this API reads.

### `MLAS` [wontfix] Modified-local-after-setjmp UB in POSIX `sigguarded`
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_posix.c.ipp:241-287`: `current.rsi` is written by `prepare_rsi` (via
the frame pointer in the signal handler, i.e. after `setjmp` executed) and then read after
`longjmp`. Per C11 7.13.2.1p3, non-volatile automatic objects modified between `setjmp`
and `longjmp` have indeterminate values after `longjmp` — this is UB (works in practice on
mainstream compilers because the frame is a memory object, but a conforming compiler may
cache `current` in registers). The struct should be `volatile` (or the members accessed
post-longjmp should be).

### `SJMS` [wontfix] _setjmp/`setjmp` selection changes signal-mask semantics
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

With `setjmp` (used when `_setjmp` is unavailable) the mask saved at the `setjmp` is
restored on `longjmp` — combined with `SA_NODEFER` handlers this can silently
unblock/block signals relative to the interrupted context. Platform-dependent.

### `PREI` [wontfix] [Windows, Low] `stdc_raise` before any `siginstall` terminates the process (unhandled SEH -> WER); POSIX returns `false`
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

On POSIX, `stdc_raise` for an uninstalled signal runs the chain in-process and returns
`false` (`thrd_signal_handle_posix.c.ipp:356-361`). On Windows the unclaimed-raise path
(`stdc_raise_initiated_exception_was_unclaimed` -> `false`, `thrd_signal_handle_windows.c.ipp:403-409,
466-473`) exists only when the library's vectored handler is installed — i.e. only after
`siginstall` (`install_sighandler_impl`, `:576-595`). A `stdc_raise(SIGUSR1, NULL, NULL)`
on a thread that has not installed the library raises the user-defined exception
`0x40000001` with no vectored handler and no enclosing library `__except`: it reaches
`UnhandledExceptionFilter` and Windows Error Reporting terminates the process, for both
user-range codes (`0x40000000|signo`) and genuine codes (SIGSEGV -> `0xC0000005`). This
contradicts the header's documented contract "returning false if we have no decider
installed for that signal" (`thrd_signal_handle.h:533-563`) on Windows, and the two
backends' `stdc_raise` behaviour differs for the identical call sequence. (The proposal
permits "implementation-defined if this function ever returns", so this is a
header-doc/backend-parity defect rather than a wording violation.)

### `WRET` [wontfix] [code-level, Windows, Med] `stdc_raise` of an *installed* signal with no decider, no guarding frame and no user `__except` terminates the process via Windows Error Reporting; POSIX hands off and returns `true`
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_windows.c.ipp:535-541`: when the map has an entry for the raised
signal but `global_handler.front == NULL` (no global deciders) and no `sigguarded`
frame covers the raise, the vectored function records the V5 dedup
(`EXCEPTION_CONTINUE_SEARCH`) and returns `EXCEPTION_CONTINUE_SEARCH` — with **no
check of `stdc_raise_initiated_exception`** (that escape hatch exists only in the
no-map-entry branch, `:456-473`). The full dispatch for
`stdc_raise(SIGTERM, NULL, NULL)` on a thread with SIGTERM installed, zero deciders,
no guarding frame and no enclosing `__try/__except` is: no library vectored
*exception* handler (only a continue handler is registered) -> no frame filter ->
the library's unhandled filter runs the (empty) global pass and returns
`CONTINUE_SEARCH` -> the vectored continue handler returns the deduped
`CONTINUE_SEARCH` -> the exception is unhandled -> Windows Error Reporting
terminates the process. `stdc_raise` never returns. The identical call sequence on
POSIX walks the (empty) frame chain, finds the map entry, and hands off to the
previously installed handler, returning `true` (`thrd_signal_handle_posix.c.ipp:404-409`)
— a hard backend-parity break of the documented contract "returning false if we
have no decider installed for that signal" (`thrd_signal_handle.h:533-563`; on
Windows it does not return at all). Distinct from `PREI` (which is the no-`siginstall`
case) and from `ZERO` (which is the case where the user's own `__except` catches the
raise and `stdc_raise` returns true): `WRET` is the *unclaimed-by-everyone* case in
between. No in-tree test exercises it — every Windows raise test either has a
claiming decider, a guarding frame, or an uninstalled signal (`thrd_signal_handle_test.c`,
`thrd_sigfpe_test.c`, `sigguarded_tss_init_test.c`, `post_uninstall_reentry_test.c`,
`stdc_raise_uninstalled_test.c`, `stdc_raise_null_info_test.c`), so the configuration
is untested. Fix direction: treat a software raise (per-thread
`stdc_raise_initiated_exception`) with a map entry but no claiming decider like the
no-entry case — set `stdc_raise_initiated_exception_was_unclaimed` and return
`EXCEPTION_CONTINUE_EXECUTION` so `RaiseException` returns and `stdc_raise` reports
false, or install a `RaiseException`-specific continuation path.

### `SUST` [wontfix] [code-level, Low] `siguninstall_system()` is a non-functional stub
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_common.ipp.ipp:555-563` — the function only validates `version == 0`
and returns 0; it installs/removes nothing. The header documents it as "Uninstall a
previously system installed signal guard", but no system installation exists anywhere in
the codebase. An API that reports success for an operation it never performs is a latent
trap for future callers (and for the eventual C standard library integration this library
targets).

### `SKIP` [wontfix] [semantics, both backends, Low] `siginstall` silently skips un-installable signals yet returns a valid handle
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

N3924 (7.14.2.5): "For all signals in the signal set `guarded`, the threadsafe
implementation shall be activated according to the Introduction above" and "If the
installation was unsuccessful, this function returns a null pointer." `siginstall`
(`thrd_signal_handle_common.ipp.ipp:505-571`) skips `SIGKILL`/`SIGSTOP` unconditionally
(`:522-524`) and the Fil-C `zis_unsafe_signal_for_handlers` signals (`:526-531`) — no
attempt to install, no error, non-NULL handle returned. The skip is invisible to the
caller: `sigfillset_asynchronous_nondebug()` includes SIGKILL and SIGSTOP on both backends
(`thrd_signal_handle_posix.c.ipp:87-93`, `thrd_signal_handle_windows.c.ipp:95`), and
`sigfillset()` (i.e. `siginstall(NULL)`) covers them on POSIX and, on Windows, also
signals 23-32 beyond MSVC's `NSIG == 23` which the `1..NSIG-1` loop never reaches. Every
`siginstall` of a full/nondebug set therefore "succeeds" while a subset of the requested
signals was never activated, and there is no way to discover which. Options: return NULL
when any member cannot be installed (and roll back — the rollback machinery already
exists), or define and document the skip; the current silent success matches neither the
wording nor the failure contract.

### `FWTF` [wontfix] [code-level, Windows + forced fallback TLS, Med] the vectored exception function NULL-derefs `tss_async_signal_safe_get(NULL)` on the fallback-TLS path
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

With `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL == 0` — i.e.
`-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` on Windows (a documented PUBLIC option) or
any non-GNU/non-MSVC Windows toolchain — `sig_tss_state_raw()` is a pointer to a static
zero-initialised `tss_async_signal_safe` handle (`thrd_signal_handle_common.ipp.ipp:328-333`),
so `*sig_tss_state_raw()` is NULL until the thread's first library call. The vectored
exception function calls `sig_global_tss_state()` on threads that may never have called
the library — the no-map-entry branch (`thrd_signal_handle_windows.c.ipp:466-468`) and
the claim-without-frame branch (`:512-513`). On the fallback path that function is
`tss_async_signal_safe_get(*sig_tss_state_raw())` (`thrd_signal_handle_common.ipp.ipp:379-385`)
→ `LOCK(mem->lock)` on the NULL handle (`tss_async_signal_safe.c.ipp:280`) — a NULL-pointer
crash *inside the exception handler*. The `tss != NULL` guards added by the X3 fix only
protect the async-safe-TLS path, where the state is a TLS pointer that can legitimately
be NULL; on the fallback path the dereference happens before the guard can see it.
Trigger: `siginstall` on one thread, then any supported-code exception (an app fault on
a signal with no map entry, or an unclaimed `stdc_raise`) on a fresh thread that has
never called the library — the process crashes where the default behaviour would be
WER/continue-search. The Windows CI never runs the fallback option (`CIGA`), so the
configuration is unexercised. Fix direction: have the fallback `sig_global_tss_state`
return NULL when `*sig_tss_state_raw()` is NULL (mirroring the async path), or check the
raw handle before calling `get`. Not reachable on macOS/POSIX (the POSIX backend only
calls `sig_global_tss_state` after `sig_global_tss_state_init`).

### `SJSP` [wontfix] _setjmp vs `setjmp` inconsistency for header-only consumers
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`WG14_SIGNALS_HAVE__SETJMP` is set only on the compiled library target
(`CMakeLists.txt:176-178`, PRIVATE). Header-only consumers never get the definition and
always use `setjmp` (saving/restoring the signal mask) even when `_setjmp` is available.
Not a correctness bug but a silent performance/behaviour split between the two modes.

### `WXMS` [wontfix] [build, minor] MSVC builds lack the `-Werror` equivalent
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`CMakeLists.txt:188-192`: GCC/Clang get `-Wall -Wextra -Wpedantic -Werror`; MSVC gets
`/W4 /experimental:c11atomics` with no `/WX`. All warnings that would break a strict
GCC/Clang build are invisible in the Windows CI leg (extends `CSTD`).

### `CXXR` [wontfix] [build, Low] the project unconditionally requires a C++ compiler for a C library
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`CMakeLists.txt:19` declares `project(wg14_signals LANGUAGES C CXX)` unconditionally (only
the *chosen* `thread_atexit` source is conditional, at `:80-84`), so a C-only toolchain
cannot configure the project on any platform — even on `__cxa_thread_atexit()` platforms
where the compiled library is all-C.

### `CIGA` [wontfix] CI gaps
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

- The Windows CI runs with `-DCMAKE_C_STANDARD` in {11,17} but MSVC ignores the C-standard
  option for `/experimental:c11atomics` in some versions.
- No CI runs the benchmark targets at all (`-E benchmark`), so the performance claims in
  the README are not verified.
- `header_only_build_test`'s single-TU C header-only consumer fails on FreeBSD —
  `current_thread_id()` returns 0 there, while the library build, the C++ and C multi-TU
  header-only builds, and `header_only_c_multi_test` all return a non-zero tid; the
  single-TU weak `_Thread_local` `current_thread_id_cached` retention is suspected. The
  test is excluded from the FreeBSD ctest run pending diagnosis; the consumer now prints
  which check failed to make the next run conclusive.
- No Windows leg runs `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON`,
  so the fallback-TLS path is never compiled or exercised with MSVC's
  `/experimental:c11atomics` (the Linux fallback legs use GCC/clang). The
  `FWTF` (also wontfix) lives precisely in that unexercised configuration, and
  `tss_async_signal_safe`'s MSVC build is otherwise only ever compiled on macOS-derived
  toolchains.

### `TRAP` [wontfix] The SIGFPE test depends on architecture trap behaviour
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`test/thrd_sigfpe_test.c:62-67` works around x64's lack of integer-divide trapping, but
the guard relies on `sigfence` and `stdc_raise(SIGFPE)` fallback, so the "real fault" SEH
path on Windows is never exercised.

### `SFAR` [wontfix] [code-level, Low] `sigfence` with more than 8 arguments produces a confusing hard error
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` (`thrd_signal_handle.h:96-106`) returns the 9th
argument as the count; `sigfence(a,...,i)` expands `WG14_SIGNALS_SIGFENCE_IMPL_i` — an
undefined identifier — yielding a cryptic compile error rather than a diagnostic about the
8-argument limit. (The 0-arg form works; verified.)
### `UNTL` [wontfix] [code-level, fallback path, Med] `siguninstall` of the last handler while another thread is inside `sigguarded` frees that thread's live state
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

On the fallback (Apple) path, the final `siguninstall` -> `sig_global_tss_state_destroy`
-> `tss_async_signal_safe_destroy` frees the shared TSS (and every per-thread entry)
*while another thread may be between `sigguarded` frames*. The interrupted thread's
`tss->front` points into the freed per-thread state; its next raise runs
`sig_global_tss_state_init` against the *dangling* `*sig_tss_state_raw()` -> UAF.
Only the fallback path is affected (the async-safe TLS path's destroy is a no-op). The
usage requirement "destroy only after all threads have left `sigguarded`" is nowhere
documented.

### `SPIN` [wontfix] Spinlock is not async-signal-safe (deadlock risk in signal handlers)
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`LOCK`/`UNLOCK` (`lock_unlock.h:33-61`) is a CAS spinlock with no signal masking, no
backoff, and no `pause`/`yield`. It is used inside signal-handler contexts on both
backends (`stdc_raise` -> `LOCK(state->lock)`, Windows vectored handler ->
`LOCK(state->lock)`, the fallback `tss_async_signal_safe_get` -> `LOCK(mem->lock)`, and —
since the abandon fix — `sigdecider_abandon`/`_resume` -> `LOCK(state->lock)`, finding
`LEAK`).
If a signal is delivered while the interrupted thread is itself holding the same lock,
the handler spins forever — a silent deadlock. The header's "usually async signal safe"
claim (`thrd_signal_handle.h:328-347`) does not cover re-entrancy.

### `ALOC` [wontfix] First signal delivery on a fresh thread performs allocation inside the handler
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`stdc_raise` -> `sig_global_tss_state_init` -> `calloc` + `thread_atexit` (which does
`std::vector` allocation / possibly throws) on the first call per thread
(`thrd_signal_handle_common.ipp.ipp:296-313`, `thread_atexit.cpp.ipp:71-85`). If the
first signal ever delivered to a thread arrives before any library call on that thread,
malloc and C++ heap operations run inside the handler (not async-signal-safe; risk of
deadlock on the heap lock). The docs recommend pre-calling `stdc_raise(0, ...)`; on Linux
this works, but the safety relies entirely on the user reading the docs.

### `REEN` [wontfix] tss_async_signal_safe_thread_init re-entrancy (signal during `attr.create`) leaks
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`tss_async_signal_safe.c.ipp:211-218` unlocks before calling the user's `create` and
re-locks before `insert`. If a signal handler runs `thread_init` on the same object in
that window (possible via `stdc_raise` -> `sig_global_tss_state_init` -> `thread_init`),
the user's `create` callback runs twice and the second `insert` replaces the first entry:
the first value leaks (no destructor on replace), `count` is double-incremented, and two
atexit registrations are queued.

### `TAFL` [wontfix] tss_async_signal_safe_thread_init does not roll back on `thread_atexit` failure
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`tss_async_signal_safe.c.ipp:228-249`: the map entry and `state->count` are committed
before `thread_atexit` is called; if it returns -1 the caller sees failure but the entry
and count remain, and no thread-exit cleanup will ever run for this thread. (On the
async-safe TLS path, `sig_global_tss_state_init` has the same pattern — it sets `*state =
mem` *before* `thread_atexit(free, mem)`; on registration failure the pointer stays set
and the next call silently succeeds with a leaked `mem`, M2.)

### `LEAK` [wontfix] [code-level, both backends, Med] `sigdecider_abandon`/`_resume` take `state->lock` inside an ASYNC-SIGNAL-SAFE API (spinlock-in-handler deadlock risk)
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

The original defect — a never-returning global decider abandoning the per-raise
`current->refcount`/`item->lifetime_refcount` references taken before the unlocked decider
call (`thrd_signal_handle_posix.c.ipp:358-366`, Windows `:404-416`), leaking the node and
container forever — is **fixed 2026-08-17 (commit `1eea326`, `sigdecider_abandon()` drops
both references; verified by the global-abandon sections of `test/guard_abandon_test.c`)**.
The follow-on fix (working tree, uncommitted) now performs the abandon/resume reference
adjustments under `state->lock` (`thrd_signal_handle_common.ipp.ipp`), removing the
unlocked non-atomic `int` `++/--` that raced concurrent raises/`siguninstall`/
`signal_decider_destroy` on other threads (a C11 data race that could free a container
under a sibling in-flight raise).

The remaining defect is that these two APIs are documented `THREADSAFE ASYNC-SIGNAL-SAFE`
(`thrd_signal_handle.h:546-567`), yet now take the `state->lock` spinlock: a decider
calling `sigdecider_abandon()` from a signal handler whose interrupted thread holds that
lock (e.g. mid-`siguninstall`/`signal_decider_destroy`) spins forever — the `SPIN` deadlock
mechanism in a documented-valid call pattern. Fix requires a lock-free atomic formulation
for the reference adjustments.

### `DEIN` [wontfix] [race, fallback path, Low] `tss_async_signal_safe_thread_deinit` reads `state->val` without the lock; concurrent `destroy` frees `mem` under it -> UAF
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`tss_async_signal_safe.c.ipp:148-152`: the thread-exit deinit reads
`struct tss_async_signal_safe_s *mem = (struct tss_async_signal_safe_s *) state->val;`
with no synchronization, then takes `LOCK(mem->lock)`. `tss_async_signal_safe_destroy`
(`:122-146`) clears `state->val` under the lock and then frees `mem`. A thread exiting
concurrently with `destroy` can read the pre-clear `state->val` (a data race on the plain
pointer), block on the lock while `destroy` runs to completion — including `free(mem)` —
and then acquire the lock on freed memory: `thread_id_to_tls_map_t_get` on the freed
table and `mem->attr.destroy(item)` through a pointer read from freed memory (arbitrary
code-execution territory). The `state->val == NULL` else-branch (`:188-201`) proves the
design intends to tolerate "destroyed while registered", but the unlocked read defeats
it. The public header's "threads must have exited first" precondition (`TSSG`) papers over
the race for consumers, but the library-internal destroy from the last `siguninstall`
(finding `UNTL`) re-opens it: a thread still registered when its TSS is destroyed exits later
and races the destroy. Related mechanism, distinct from finding `UNTL`'s dangling-`tss->front`
failure; both share the trigger.

### `SIGF` [wontfix] Data race in the `sigfillset_*` lazy initialisation
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_posix.c.ipp:58-76` double-checks `sigismember(&v, signos[0])` then
writes `v = x` without a lock or atomics. Two threads calling `sigfillset_synchronous`
concurrently both compute `x` and both store `v` — a benign write-write race on identical
values, but still a data race under the C memory model (UB), and the "is it initialised?"
check reads a non-atomic while another thread writes. In practice the sigset write is
aligned and atomic; the sets also carry `__attribute__((constructor))`, which
pre-initialises the statics at load time for executables and substantially mitigates the
race — but the attribute is POSIX-only, so the race is unmitigated on Windows for
`synchronous_sigset`/`asynchronous_nondebug_sigset`.

### `CPPD` [wontfix] [C++ consumers, Low] longjmp across objects with non-trivial destructors is UB
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`stdc_raise`'s `invoke_recovery` path (`thrd_signal_handle_posix.c.ipp:332`) and the
Windows vectored handler's `longjmp` (`thrd_signal_handle_windows.c.ipp:440`) skip C++
destructors for any automatic object live in the guarded frame — UB per the C++ standard.
The library is documented as C++-usable, and MSVC explicitly disables warning 4611
(`thrd_signal_handle_windows.c.ipp:46`) for it. A C++ `sigguarded` caller with RAII objects
in scope of a raised signal gets skipped destructors silently.

### `SFNK` [wontfix] [race, Windows, Low] `sigfence`'s MSVC fallback shares a process-static sink array; concurrent calls race on `sigfence_sink[8]`
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

The GNU/Clang asm path (`thrd_signal_handle.h:171-201`) is a pure `__asm__` with no shared
mutable state. The MSVC (and `DISABLE_INLINE_ASM`/Fil-C) fallback (`:202-311`) uses a
per-TU `static void *volatile WG14_SIGNALS_PREFIX(sigfence_sink)[9]`; each fenced
variable writes its own slot (`0..7`) but `WG14_SIGNALS_SIGFENCE_BARRIER()` does
`sigfence_sink[8] = sigfence_sink[8]` (`:218-220`) — a non-atomic read-modify-write of a
shared static executed by every thread on every `sigfence` call. Two threads calling
`sigfence` concurrently is a C11 data race (UB in the abstract machine; benign torn-read
in practice on x64/ARM64). The proposal declares `sigfence` async-signal-safe; the
fallback's first call inside a handler while the interrupted thread was mid-`sigfence` is
exactly the re-entrant case. Fix direction: make the sink per-thread
(`WG14_SIGNALS_THREAD_LOCAL`), which removes both the cross-thread race and the
handler-reentrancy read of the interrupted value.

### `FLGS` [wontfix] SA_NOCLDWAIT + `SA_NODEFER` + missing `SA_RESTART` alter process semantics
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

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

### `TAOM` [wontfix] thread_atexit C++ exceptions disabled -> OOM terminates
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thread_atexit.cpp.ipp:71-85`: with `-fno-exceptions` the `try/catch` block is compiled
out; `std::vector::emplace_back` on allocation failure calls `std::terminate` instead of
returning -1. The library is designed to be embedded in C standard libraries where
exceptions may be disabled; this path then crashes instead of reporting failure.

### `FPUN` [wontfix] Function-pointer type pun for atexit callback
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`tss_async_signal_safe.c.ipp:246-248` casts `int (*)(struct deinit_state *)` to
`void (*)(void *)` and registers it via `thread_atexit`. Calling through an incompatible
function-pointer type is UB per the C standard (works on common ABIs, but a latent
portability hazard).

### `MAST` [wontfix] pthread_getthreadid_np / `mach_thread_self` are not async-signal-safe
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`current_thread_id.c.ipp:80-83`: the Apple branch performs `mach_port_deallocate` (kernel
round-trip) on every cache miss; `current_thread_id()` is documented as "ASYNC SIGNAL
SAFE" (`current_thread_id.h:64-65`). On fallback platforms the cache miss happens inside a
signal handler on first use -> async-signal-unsafe syscalls.

### `CRTS` [wontfix] MSVC CRT signals bypass SEH (Windows)
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`siginstall` on Windows installs only SEH vectored handlers
(`thrd_signal_handle_windows.c.ipp:486-505`); it does not install CRT signal handlers.
`abort()`, `raise(SIGFPE)`, `assert`, etc. on MSVC dispatch through the CRT, which does
not raise SEH exceptions — so they never reach the library. Only genuinely SEH-raised
exceptions (access violations, integer overflow traps on x86, explicit `RaiseException`)
are handled. A substantial functional gap on Windows versus POSIX, and it is not
documented.

### `MING` [wontfix] Mingw
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

Deliberately unsupported (`#error` at `thrd_signal_handle_windows.c.ipp:273-275`), but
before reaching that `#error` the header has already redefined `sigset_t` on `_WIN32`
(`thrd_signal_handle.h:43`), which collides with MinGW's own `sigset_t` typedef — the
first of several Mingw incompatibilities.

### `FORK` [wontfix] [code-level, Low] no `pthread_atfork` handling; stale TID caches are inherited across `fork()`
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

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

### `NSIG` [wontfix] [code-level, Low] `NSIG` is not POSIX-mandated; a missing `NSIG` silently disables `siginstall`
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_common.ipp.ipp:61-62` uses `#if NSIG < 1024` (undefined `NSIG`
evaluates to 0), and the `siginstall`/`siguninstall`/decider loops iterate `1 .. NSIG-1` —
with `NSIG` undefined the loops never execute and `siginstall` **returns success having
installed nothing**. All CI platforms define NSIG, so this is exotic-POSIX-only, but the
failure is silent.

### `NDBS` [wontfix] [code-level, Windows, Low, extends X9] `asynchronous_nondebug_sigset` silently omits most documented signals and includes the two `siginstall`-skipped ones
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_windows.c.ipp:92-108` builds the nondebug set from only
`{SIGINT, SIGKILL, SIGSTOP, SIGTERM}`, whereas the header documents it as containing at
least SIGALRM, SIGCHLD, SIGCONT, SIGHUP, SIGINT, SIGKILL, SIGSTOP, SIGTERM, SIGTSTP,
SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGPOLL, SIGPROF, SIGURG, SIGVTALRM. Additionally
`SIGKILL` and `SIGSTOP` (which `siginstall` deliberately skips) are in the set, so
`siginstall(sigfillset_asynchronous_nondebug())` claims two signals that are never
installed. The debug set is the sibling defect: `asynchronous_debug_sigset`
(`:116-123`) returns the *empty* set while the header documents "at least these POSIX
signals are within this set: SIGQUIT, SIGTRAP, SIGXCPU, SIGXFSZ" — a Windows consumer gets
a guard set that never matches anything. **Extended:** the same family
covers `sigfillset_synchronous` on Windows: `synchronous_sigset` (`:69-85`) is
`{SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV}`, i.e. 5 of the 7 members the header's "at
least these POSIX signals are within this set: SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGPIPE,
SIGSEGV, SIGSYS" list promises — SIGPIPE and SIGSYS are undefined on MSVC and are simply
omitted. All three Windows filler sets therefore deliver less than their documented
"at least" lists.

### `UECL` [wontfix] [code-level, Windows, Low] `stdc_raise`'s user-range exception codes collide with the documented user-defined exception-code range
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_windows.c.ipp:139, 160-172` — signals with no native Win32 code are
raised as `WG14_SIGNALS_USER_RAISE_BASE | (signo & 0x3FFFFFFF)`, i.e.
`0x40000001..0x40000020` for signos 1..32 — exactly the documented user-defined
exception-code range (`0x40000000`-`0x7FFFFFFF`). `signal_from_win32_exception_code`
(`:208-219`) reverse-maps *any* exception whose code is in that range to a signo, so an
application raising its own exception `0x40000001..0x40000020` for its own purposes (a
documented and recommended Windows idiom) is indistinguishable from a library raise and
is routed through the library: a `sigguarded` frame guarding the mapped signal runs its
frame decider and may `longjmp`-recover over the application's exception, a global
decider may claim it (swallowing it), and `stdc_raise`'s own marker/context parameters
are absent so deciders see `error_code == 0` etc. The library cannot distinguish the
two; the collision range could be narrowed (the signo mapping only needs 5 bits, e.g.
`0x40000100 | signo`), or the vectored function could consult a per-thread
"library raise in progress" marker before treating a user-range code as a library raise.
No memory-safety consequence; a behavioural hijack of the host application's own
exception codes.

### `CXAT` [wontfix] [code-level, Low] the C `thread_atexit` silently swallows a failed `__cxa_thread_atexit()` registration
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thread_atexit.c.ipp:66-76` — the C implementation (used whenever
`__cxa_thread_atexit()` is available, i.e. the glibc/macOS/FreeBSD builds) calls
`__cxa_thread_atexit(func, obj, &thread_atexit_dso_symbol)` and unconditionally
`return 0`, ignoring the return value. The comment justifies this because macOS's
`__cxa_thread_atexit` return is unreliable, but on glibc it is the reliable
`__cxa_thread_atexit_impl` wrapper (libsupc++/libc++) that genuinely returns -1 on
ENOMEM. A dropped registration means the registered deinit never runs at thread exit:

1. `tss_async_signal_safe_thread_init` returns success (its `res = thread_atexit(...)`
   is always 0) while no thread-exit cleanup is scheduled — the per-thread map entry and
   the shared `deinit_state` leak, exactly the state `TAFL`/M2 describe, but with no
   failure ever surfaced to the caller (the `TAFL` rollback is unreachable on this path).
2. `sig_global_tss_state_init` (async-safe TLS path,
   `thrd_signal_handle_common.ipp.ipp:296-313`) sets `*state = mem` before calling
   `thread_atexit(free, mem)`; an invisible registration failure leaves `*state` set and
   the `calloc`'d state leaked at thread exit.

Fix direction: probe at configure time whether the platform's `__cxa_thread_atexit`
return is reliable (the CMake `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` probe already
exists) and propagate the return on platforms where it is, keeping the ignore-everything
behaviour only for the known-unreliable ones (macOS).

### `SFQL` [wontfix] sigfence on GNU compilers requires lvalues; non-lvalues fail to compile
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`WG14_SIGNALS_SIGFENCE_IMPL_1(a)` expands to `__asm__ volatile(";" : "+m"(a) : : "memory")`.
The `+m` operand must be an lvalue: `sigfence(42)` or `sigfence(x + 1)` is a hard compile
error. Verified (`error: invalid lvalue in asm output`); the 0-arg form compiles. The
header now documents the requirement ("Any variable in the argument list MUST be a
lvalue", `thrd_signal_handle.h:248-249`), so the failure is documented if still a bare
compiler error.

### `TSSD` [wontfix, code-level, security, Low] `tss_async_signal_safe_destroy` double-destroy is an unguarded use-after-free

`tss_async_signal_safe.c.ipp:114-138`: a **second** `destroy` on the same (non-NULL)
handle calls `LOCK(mem->lock)` on freed memory and then `free(mem)` again — a double-free /
use-after-free, i.e. a potential security vulnerability. Post-destroy `get`/`thread_init`
on the freed handle crash identically. (AGENTS.md rule 9: NULL/zeroed *input* handles
crashing is intended fail-fast and is not a defect; the freed-handle double-destroy is
out of scope of that rule because it is a use-after-free, not a NULL input.)

**Wontfix rationale (adjudicated 2026-08-16).** Destroying an already-destroyed
identifier is undefined behaviour under the governing contracts: C11 7.26.6.3
`tss_delete` and POSIX `pthread_key_delete` define only a single deletion, and the N3924
wording's `tss_async_signal_safe_destroy` (7.30.6.6, `docs/proposed-wording.md`) does the
same. Standard-library implementations tolerate a second delete only because their keys
are indices into process-lifetime storage that deletion never frees: glibc's
`tss_delete` -> `pthread_key_delete` validates the key against the never-freed
`__pthread_keys[PTHREAD_KEYS_MAX]` slot array and returns `EINVAL` for a stale key (the
void `tss_delete` silently drops it), musl's delete zeroes the slot's `seq` and is a
repeatable no-op, and Windows `TlsFree` validates a DWORD index against a
process-lifetime bitmap. This reference implementation's handles are raw pointers to
allocations that `destroy()` frees, so a guard would require emulating those slot tables
(index/generation handles, or never-freed/recycled instance storage) — API churn and
process-lifetime memory retention out of proportion to a caller-contract violation the
standards leave undefined. Resolution: a second `destroy` on the same value is
**documented undefined behaviour** (fail-fast, in the spirit of AGENTS.md rule 9); the
value is left dangling after `destroy` and must not be reused (see
`tss_async_signal_safe.h` and finding `TSSG`). No guard will be added.

### `DEDE` [wontfix, code-level, Low] `signal_decider_destroy` double-destroy is an unguarded use-after-free

`thrd_signal_handle_common.ipp.ipp:757` (`free(p)`) unconditionally frees the handle. A
second `signal_decider_destroy` on the same pointer reads freed memory before the
double-free. No guard exists.

**Wontfix rationale (adjudicated 2026-08-16).** As for `TSSD`, destroying an
already-destroyed handle is undefined behaviour: the N3924 wording's
`signal_decider_destroy` (7.14.2.8) defines only a single destroy — its thread-safety
clause covers concurrent calls on *different* handles, and the header's THREADSAFE NOT
REENTRANT note does not address double-destroy — and the C11/POSIX stdlib contracts for
the analogous key-deletion functions likewise leave a second delete undefined.
Standard-library tolerances (glibc/musl fixed slot arrays, Windows `TlsFree` bitmap) all
rest on keys being indices into storage that deletion never frees; this implementation's
handle is a raw pointer that is freed, and guarding it would need the same slot-table
redesign rejected for `TSSD`. Resolution: a second `signal_decider_destroy` on the same
handle is **documented undefined behaviour** (fail-fast); see `thrd_signal_handle.h`. No
guard will be added.

## 2. Async-signal-safety claims vs reality

The public API makes strong claims ("ASYNC-SIGNAL-SAFE", "USUALLY ASYNC-SIGNAL-SAFE").
Verdicts:

- `tss_async_signal_safe_get`: NOT safe in the general case. It takes a spinlock (`SPIN`),
  reads a cached thread ID (fine once populated), and on cache-miss performs
  async-signal-unsafe syscalls on Apple (`MAST`). Safe only when (a) the TLS cache was
  primed outside the handler and (b) no other thread/handler holds the object's lock.
- `current_thread_id`: safe on Linux/ELF (initial-exec TLS; the `gettid` syscall is
  signal-safe) and Windows; unsafe on Apple on first use per thread (`MAST`).
- `sigfillset_*`: safe (read-only static init with a benign double-checked write race —
  see `SIGF`).
- `sigguarded` / `stdc_raise`: "usually safe" only after the per-thread setup call
  (`stdc_raise(0, ...)`). On Linux with the documented pre-call, the happy path is
  signal-safe.
- `siginstall` / `siguninstall` / `signal_decider_create` / `signal_decider_destroy`:
  NOT async-signal-safe (malloc, fprintf, locks) — correctly documented as
  THREADSAFE-only.

---

## 3. Minor issues and observations

- `siguninstall`'s `-1` failure path (`thrd_signal_handle_common.ipp.ipp:545-548`) leaks
  `ss`; and since `uninstall_sighandler` always returns `true`, the error path is dead
  code.
- `signal_decider_destroy` acquires `state->lock` per signal (NSIG iterations), even for
  signals not in the guarded set — O(NSIG) lock round-trips for a single destroy.
- `install_sighandler` never flushes `deferred_frees` (only uninstall/destroy do), so
  deferred nodes are held until an uninstall — a minor memory retention.
- `siginstall` returns the allocated `sigset_t *`; there is no way to uninstall by signal
  subset, and `siguninstall` frees the passed pointer, making double-uninstall a UAF
  (user error, undocumented). **Extended:** the docs never state that the pointer
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
- **AC1 [docs-hygiene, Low]** source comments and the CI YAML cite finding IDs from the
  pre-renumbering scheme (`analysis.md 5.10`, `2.9`/`W11`, `1.8`/`C3`/`Y10`, cited by
  `test/thrd_sigfpe_test.c`, `test/sigfence_fence_test.c`, `test/header_only_build_test.cmake`,
  `test/header_only_c_consumer/main.c`, `ci.yml`) that no longer resolve in this document;
  retarget them to the surviving codes or to `ci.yml` itself.
- `sigguarded`'s doc `\return` ("The value returned by `guarded`, or `recovery`",
  `thrd_signal_handle.h:502`) is sloppy: `recovery` is the *value returned by the
  recovery function*, and the failure return (`-1`, finding `AMBI`) is not mentioned.
- `tss_async_signal_safe_thread_init`'s create-failure path (`tss_async_signal_safe.c.ipp:222-226`)
  returns the callback's error code verbatim without setting `errno`, so a caller that
  checks `errno` (as the `stdc_raise` setup path teaches) sees stale `errno` — the
  wording's `thrd_error` contract (any nonzero) is met, but the failure diagnostic is not.
- `thrd_signal_handle.h:403-406` documents the POSIX `stdc_raise(signo, NULL, NULL)`
  `raw_info == NULL` behaviour, but on Windows the same call always yields a non-NULL
  `raw_info` (the `EXCEPTION_RECORD`) — the note covers this ("on Windows the OS info is
  always present"); verified consistent.
- **CMake probe-cache staleness.** `check_c_source_compiles` caches its results by
  variable name: `LIBC_HAS__SETJMP` (`CMakeLists.txt:24`), `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT`
  and the per-candidate `_wg14_signals_cxa_result` (`:52-64`), and
  `WG14_SIGNALS_FT_PROBE_0..4` (`:142-154`). The cached values persist across
  reconfigures; switching the compiler (or the libc) in the same build directory
  reuses the stale probe outcomes with no diagnostic — the feature-test-macro ladder
  (`_ftm_choice`), the `_setjmp` selection and the `__cxa` path choice can all silently
  come from a previous toolchain. Fix direction: `unset(... CACHE)` the probe
  variables at the top of each configure, or key them on `CMAKE_C_COMPILER`.
- **POSIX `stdc_raise` dead store.** `thrd_signal_handle_posix.c.ipp:332` — the
  `sig_decision_resume_execution` case executes `frame = frame->prev;` immediately
  before `return true;`; the store is dead. Harmless, but it reads as if the walk
  continued; drop the assignment.
- **Windows `install_sighandler_impl` overwrites an already-present unhandled-exception
  filter at install time.** `:591-592` unconditionally replaces the process filter and
  only restores the pre-library one at the *final* `siguninstall` (`UFLT`), so an app that
  installs its filter after the first `siginstall` loses it even while other signals
  remain installed — the UFLT finding covers the uninstall-time clobber; this is the
  install-time overwrite of an already-present filter. (POSIX has no analogue: `sigaction`
  chaining preserves the old handler.)
- **`siginstall_rollback_test` is excluded from Fil-C builds:** `--wrap` is unusable under
  Fil-C's symbol pizlonation (the linker's rewrites never match the mangled names), so the
  test is excluded (`test/CMakeLists.txt`, `CMAKE_C_COMPILER MATCHES "[Ff][Ii][Ll]-?[Cc]"`);
  rollback coverage remains on the GCC/Clang Linux and FreeBSD legs. (The earlier
  `--wrap` + hidden-visibility link breakage that motivated the exclusion was fixed by
  giving the interposer default visibility.)

### Minor proposal-conformance notes (N3924 rev 4 wording)

- **Async-signal-safe claims.** The wording marks `sigguarded` and `stdc_raise`
  "thread-safe and async-signal-safe" (7.14.4.1, 7.14.3.2). The implementation documents
  both as "USUALLY ASYNC-SIGNAL-SAFE" with a per-thread pre-call requirement
  (`thrd_signal_handle.h:499-518, 533-563`); the mechanisms that break the bare claim are
  findings `SPIN`, `ALOC`, `TLSD`, `MAST`. The reference implementation therefore does
  not yet satisfy the
  wording's claim on first use per thread — the header's honesty is the mitigation.
- **`sigguarded` with `recovery == NULL`.** Allowed by both backends (POSIX falls through
  to outer frames/globals, `thrd_signal_handle_posix.c.ipp:335-343`; Windows
  `EXCEPTION_CONTINUE_SEARCH`, `thrd_signal_handle_windows.c.ipp:273-279`) and exercised
  by `recovery_null_loop_test`, but neither the header nor the wording defines it; the
  wording says the recovery function "shall be called".
- **`tss_async_signal_safe_thread_init` return values.** The wording promises
  `thrd_success`/`thrd_error`; the implementation propagates the user `attr->create`
  callback's arbitrary nonzero return verbatim (`tss_async_signal_safe.c.ipp:222-226`), so
  a create returning e.g. 1 yields thread_init returning 1 (neither 0 nor -1).
- **Windows `stdc_raise` drops `raw_context` on a full `EXCEPTION_RECORD`.** When
  `NumberParameters + 2 > EXCEPTION_MAXIMUM_PARAMETERS` the marker append is skipped
  (`thrd_signal_handle_windows.c.ipp:384-391`) and the decider's `raw_context` silently
  becomes the raise-time `ContextRecord` instead of the caller's context. Extends `MUTR`.
- **`sighandler_info` / `stdc_siginfo` field-name drift.** The struct members match the
  wording (`signo`, `error_code`, `addr`, `value`, `raw_info`, `raw_context`) — checked,
  no deviation; recorded here only to note it was verified.

---

## 4. Priority-ordered remediation summary

The severity label in the tables classifies each finding's worst-case impact; the row
order is the remediation priority, which applies the criteria below to the label *and*
the finding's trigger likelihood. It is not a strict severity sort: the 37 adjudicated
wontfix findings rank *last* regardless of severity, and the Med items
`TIDR`, `TLSD` and `TOPL` follow Low items that are more dangerous in practice.
The main table lists the fixable findings; the wontfix findings have their own
summary table below it. The ranking criteria below describe the original priority
assessment; items adjudicated wontfix (see tier 13) are retained in the
wontfix table regardless of where their tier placed them.

Where merged findings touch the same machinery (`DEIN` extends the `UNTL` trigger,
`ZERO`-`PRCR` are the
`stdc_raise`/`siginstall`/decider contract family), the earlier finding is referenced
rather than duplicated. Every finding is cited by its stable four-letter code.

### Priority ordering rationale

Ranking criteria, applied in order of precedence:

1. **Memory corruption in the live raise path.** UAF / dangling frame state / UB that
   corrupts memory or crashes during ordinary use of the core guarded-raise machinery
   (`UNTL`). Within the tier: the deterministic UAF (`UNTL`) leads. (The never-returning
    decider frame-pinning family — POSIX `JLGS` and its Windows sibling `NDEC` — was fixed
    and removed: `JLGS` 2026-08-17, `NDEC` in the working tree 2026-08-18.)
2. **Violation of the library's core contract: async-signal safety.** Deadlock from the
   spinlock in handler context (`SPIN`) and allocation inside the handler on first use
   (`ALOC`). Ranked below tier 1 because both need timing/re-entrancy conditions or
   non-default user setup to trigger, whereas tier 1 defects fire on plausible ordinary
   usage.
3. **Unbounded resource/lifecycle leaks.** No crash today but progressive degradation:
   re-entrancy double-create (`REEN`) and missing rollback on `thread_atexit` failure
   (`TAFL`). (`LEAK`, the never-returning-decider refcount leak, was fixed by
   `sigdecider_abandon`; its residual — a spinlock taken inside an ASYNC-SIGNAL-SAFE API —
   is the `SPIN` mechanism, and is listed there.)
4. **Data races and setjmp-family UB.** C/C++ memory-model violations that current
   compilers tolerate today. The thread-exit deinit reading `state->val` unlocked against a
   concurrent destroy (`DEIN`) leads the tier; then the benign double-checked `sigfillset`
   write race (`SIGF`), modified-local-after-setjmp (`MLAS`), `_setjmp`/`setjmp` mask
   semantics (`SJMS`), skipped C++ destructors (`CPPD`), and the `sigfence` MSVC fallback's
   shared-sink race (`SFNK`). Latent UB, not today's crashes.
5. **Silent alteration of host-process semantics.** POSIX install flags and
   default-action handling that change the behaviour of the host process for the whole
   tenure of an install: `SA_NOCLDWAIT`/no `SA_RESTART`/`SA_NODEFER` on the default
   `siginstall(NULL)` path used by every test and the README (`FLGS`), unknown-signal
   fallback (`UNKN`), and stale TID reuse (`TIDR`). Default-path triggers precede rarer
   ones. (The stop/continue and realtime re-raise discarding the library's handler —
   `DFLT` — was fixed: `invoke_sigaction` now restores the installed handler after the
   default action.)
6. **Error-path and lock hygiene.** Allocation-failure correctness and lock discipline in
    non-hot paths (`SDCF` `signal_decider_create` failure accounting, `TAOM` OOM terminate
    with exceptions disabled, `FPUN` function-pointer type pun).
7. **API-contract and error-reporting violations.** Documented behaviour is not
   delivered, including the N3924 wording's return contracts. Leads with the findings
   that can crash or silently lie about what happened: NULL `siginfo_t`/`ucontext_t`
   hand-off that can crash a pre-existing `SA_SIGINFO` handler (`NSIH`), `stdc_raise`
   reporting success for a silently-ignored signal (`IGND`), `stdc_raise` returning true
   when zero deciders were called though the wording requires false (`ZERO`), Windows
   `stdc_raise` before any `siginstall` terminating the process via WER while POSIX
   returns false (`PREI`), the no-op `siguninstall_system` stub (`SUST`), `siginstall` silently
   skipping SIGKILL/SIGSTOP/Fil-C signals yet reporting success (`SKIP`),
   `signal_decider_create` before `siginstall` silently losing the decider (`PRCR`), and
   `sigguarded`'s failure value colliding with a legitimate -1 (`AMBI`). The iteration-order
   deviation (`ORDR`) follows: its trigger is exotic and the wording contradicts its own
   `signal_decider_create` description, so it awaits a WG14 decision. The three public
   identifier/signature deviations from the wording (`ENUM` enum member name, `TYDF` error-code
   typedef name, `VSDT` Windows sigset helpers returning `void` instead of `int`) close the
   tier: they are loud compile-time failures, not silent ones.
8. **Portability and platform gaps.** Compile failures or safety-claim gaps outside the
   exercised CI matrix. The Med items lead this tier because they also break the
   async-safety claim or are real functional gaps on a supported platform: optimistic
   async-safe-TLS auto-detection (`TLSD`), `mach_thread_self`/`pthread_getthreadid_np` not
   async-safe on Apple (`MAST`), and MSVC CRT signals bypassing SEH (`CRTS`). The rest are
   exotic-platform build/portability risks (musl `struct __siginfo`, `ucontext_t`
   spelling, MinGW, `_setjmp`/`setjmp` split, missing signal guards, `fork()` TID
   caches, undefined `NSIG`, glibc-internal and realtime signal range).
9. **Build-system issues.** Configuration/package defects, ranked before test issues
   because they gate what CI can observe: `PROJECT_IS_TOP_LEVEL` silently disabling the
   whole test suite on CMake 3.15-3.20 (`TOPL`), MSVC missing `/WX` (`WXMS`), the unconditional
   CXX requirement (`CXXR`), the undeclared C++ runtime dependency (`CPPR`), the unused
   `CMAKE_C_STANDARD` cache variable (`CSTD`), CI gaps (`CIGA`), and phantom export names
   (`PCFG`).
10. **Test-harness defects.** Flaws in the test shims that can mask real failures or
     write out of bounds: `thrd_join`/`thrd_create` (`THRD`), test storage exhaustion (`STOR`),
     the trap-dependent SIGFPE test (`TRAP`), and missing failure/edge API coverage (`TCOV`).
11. **Minor, cosmetic, and Windows edge-case quirks.** Behavioural divergences with low
    practical impact: the cryptic `sigfence` 9-argument error (`SFAR`) first as it affects
    every user's compile experience, then the Windows-specific quirks (`SABA` SIGABRT
    non-continuable, `MUTR` mutated `EXCEPTION_RECORD`, `UFLT` clobbered unhandled filter,
    `NDBS`/`GDIR`/`MSQR`/`CATM`/`SFPD` set and `error_code` divergences).
12. **Documented limitations and open design notes.** Behaviour that is either explicitly
    documented or needs a design decision before a fix is worthwhile: the unlocked
    `attr.create` callback (`UCRE`), the swallowed `__cxa_thread_atexit` failure on the C
    path (`CXAT`), the now-documented `sigfence` lvalue requirement (`SFQL`), and the
    documented precondition deviations of `tss_async_signal_safe_destroy` and
    `tss_async_signal_safe_get` (`TSSG`).
13. **Adjudicated wontfix.** Thirty-seven findings were adjudicated not to fix:
    the security-class double-destroy pair `TSSD`/`DEDE` (2026-08-16 — double-
     destroy is undefined behaviour under the C11/POSIX/N3924 contract and, as this
     reference implementation keeps raw-pointer handles that `destroy()` frees, no guard
     will be added; see the heading bodies), on 2026-08-17: `UNTL`, `SPIN`, `ALOC`,
     `REEN`, `TAFL`, `LEAK`, `DEIN`, `SIGF`, `CPPD`, `SFNK`, `FLGS`, `TAOM`, `FPUN`,
     `MAST`, `CRTS`, `MING`, `FORK`, `NSIG`, `NDBS`, `UECL`, `CXAT`, `SFQL`, `DEDE`, and
     on 2026-08-18: `MLAS`, `SJMS`, `PREI`, `WRET`, `SUST`, `SKIP`, `FWTF`, `SJSP`,
     `WXMS`, `CXXR`, `CIGA`, `TRAP`, `SFAR`.
     Because no remediation is expected for them, they rank **last**, below every
     fixable finding, despite some being among the higher-severity or weaponisable
     items in the inventory.

Tie-breakers within a tier: severity label; then whether the trigger is a realistic user
pattern versus an exotic or timing one (confirmed reproductions and deterministic
corruption beat narrow-window UB); then confirmed reproduction over code-level analysis;
then backend scope.

| Code | Category | Severity | Issue |
|---|---|---|---|
| `UNKN` | semantics | Low | `raw_signal_handler` unknown signals |
| `TIDR` | identity | Med | thread-ID reuse with stale entries |
| `SDCF` | error | Low | `signal_decider_create` failure path |
| `UCLK` | locking | Low | `tss_async_signal_safe_destroy` runs user `attr.destroy` under `mem->lock` (re-entrancy deadlock) |
| `NSIH` | contract | Low-Med | NULL `siginfo` hand-off to pre-existing SA_SIGINFO handler |
| `IGND` | contract | Low | `stdc_raise` true when previous handler ignored |
| `ZERO` | contract | Low | `stdc_raise` returns true with zero deciders called (wording: false) |
| `WFRM` | windows | Med | global deciders run before thread-local frame deciders on Windows; frame deciders skipped for software raises of uninstalled signals |
| `PRCR` | contract | Low | `signal_decider_create` before `siginstall` silently loses the decider |
| `AMBI` | contract | Low | `sigguarded` -1 indistinguishable from legit -1 |
| `ORDR` | semantics | Med | decider iteration order is the inverse of the 7.14.1 ordering clauses (wording self-contradictory) |
| `ENUM` | api | Low | enum member `sig_decision_invoke_recovery` vs wording's `sig_decision_call_recovery` |
| `TYDF` | api | Low | typedef `thrd_raised_signal_error_code_t` vs wording's `stdc_siginfo_error_code_t` |
| `VSDT` | api | Low | Windows sigset helpers `void` vs wording's `int` (and POSIX "always returns zero" not guaranteed) |
| `TLSD` | portability | Med | async-safe TLS detection too optimistic; forced-on-Apple unsafe |
| `PTHD` | portability | Med | pthread-key `thread_atexit` fallback drops every callback on Darwin (TLS torn down before key destructors) **[confirmed]** |
| `MUSL` | portability | Low-Med | `siginfo_t`/`ucontext_t` dispatch breaks musl |
| `SIGN` | portability | Low | missing `SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards |
| `NEGS` | ub | Low | out-of-range/negative `signo` UB frame walk |
| `RTIM` | semantics | Low | `siginstall(NULL)` installs glibc-internal/realtime |
| `TOPL` | build | Med | `PROJECT_IS_TOP_LEVEL` needs CMake >= 3.21 |
| `CPPR` | build | Low | C++ runtime dependency undeclared |
| `CSTD` | build | Low | `CMAKE_C_STANDARD` unused for consumers |
| `PCFG` | build | Low | `ProjectConfig.cmake.in` non-existent exports |
| `THRD` | test | Low | `thrd_join`/`thrd_create` harness defects |
| `STOR` | test | Low | test storage exhaustion |
| `TCOV` | test | Low | no failure/edge API coverage |
| `SABA` | windows | Low | `stdc_raise(SIGABRT)` non-continuable |
| `MUTR` | windows | Low | `stdc_raise` mutates caller's `EXCEPTION_RECORD` |
| `UFLT` | windows | Low | `siguninstall` clobbers app filter |
| `GDIR` | contract | Low | global decider `invoke_recovery` divergence |
| `MSQR` | windows | Low | user `EXCEPTION_RECORD` params masquerade |
| `CATM` | cpp | Low | `calloc` C++ `std::atomic_uint` lifetime |
| `SFPD` | windows | Low | `stdc_raise(SIGFPE)` code divergence |
| `DRGN` | windows | Low | drain-time re-registration dropped on MSVC TLS-directory path; UB on C++ vector path |
| `UCRE` | contract | Low | `thread_init` unlocked `attr.create` |
| `TSSG` | docs | Low | destroy precondition stricter than wording; get precondition undocumented |
| `DECR` | docs | Low | `signal_decider_create` doc describes stale bool decider contract |

### Wontfix findings (adjudicated not to fix)

| Code | Category | Severity | Issue |
|---|---|---|---|
| `MLAS` (wontfix) | ub | Low | modified-local-after-setjmp UB |
| `SJMS` (wontfix) | semantics | Low | `_setjmp`/`setjmp` mask semantics |
| `PREI` (wontfix) | windows | Low | Windows `stdc_raise` pre-install terminates via WER; POSIX returns `false` |
| `WRET` (wontfix) | windows | Med | `stdc_raise` of installed signal with no decider/frame/user `__except` terminates via WER; POSIX hands off and returns `true` |
| `SUST` (wontfix) | contract | Low | `siguninstall_system` no-op stub |
| `SKIP` (wontfix) | contract | Low | `siginstall` silently skips SIGKILL/SIGSTOP/Fil-C signals, still returns success |
| `FWTF` (wontfix) | windows | Med | fallback-TLS `sig_global_tss_state()` NULL-deref in vectored function on fresh threads |
| `SJSP` (wontfix) | portability | Low | `_setjmp` vs `setjmp` inconsistency |
| `WXMS` (wontfix) | build | Low | MSVC lacks `/WX` |
| `CXXR` (wontfix) | build | Low | project requires CXX |
| `CIGA` (wontfix) | build | Low | CI gaps (Windows C_STANDARD, benchmarks, FreeBSD) |
| `TRAP` (wontfix) | test | Low | SIGFPE test trap-dependent |
| `SFAR` (wontfix) | header | Low | `sigfence` >8 args cryptic error |
| `UNTL` (wontfix) | memory | Med | `siguninstall` during another thread's `sigguarded` frees live TSS (fallback) |
| `SPIN` (wontfix) | async | Med | Spinlock not async-signal-safe |
| `ALOC` (wontfix) | async | Med | first delivery allocates in handler |
| `REEN` (wontfix) | leak | Low | `thread_init` re-entrancy leaks |
| `TAFL` (wontfix) | leak | Low | `thread_init` no rollback on `thread_atexit` failure |
| `LEAK` (wontfix) | race | Med | `sigdecider_abandon`/`_resume` take `state->lock` inside an ASYNC-SIGNAL-SAFE API (spinlock-in-handler deadlock risk) |
| `DEIN` (wontfix) | race | Low | deinit reads `state->val` unlocked; destroy race -> UAF (extends `UNTL`) |
| `SIGF` (wontfix) | race | Low | `sigfillset_*` lazy-init data race |
| `CPPD` (wontfix) | cpp | Low | longjmp skips C++ destructors |
| `SFNK` (wontfix) | race | Low | `sigfence` MSVC fallback shared-sink data race |
| `FLGS` (wontfix) | semantics | Med | `SA_NOCLDWAIT`/`SA_NODEFER`/no `SA_RESTART` |
| `TAOM` (wontfix) | error | Low | `thread_atexit` C++ exceptions disabled -> OOM terminates |
| `FPUN` (wontfix) | ub | Low | fn-pointer type pun |
| `MAST` (wontfix) | async | Med | `pthread_getthreadid_np`/`mach_thread_self` not async-safe |
| `CRTS` (wontfix) | windows | Med | MSVC CRT signals bypass SEH |
| `MING` (wontfix) | portability | Low | Mingw |
| `FORK` (wontfix) | portability | Low | no `pthread_atfork`; stale TID across `fork()` |
| `NSIG` (wontfix) | portability | Low | missing `NSIG` silently no-ops `siginstall` |
| `NDBS` (wontfix) | windows | Low | nondebug set omits signals/includes SIGKILL |
| `UECL` (wontfix) | windows | Low | `stdc_raise` user-range codes collide with app user-defined exception codes |
| `CXAT` (wontfix) | error | Low | C `thread_atexit` swallows failure |
| `SFQL` (wontfix) | header | Low | `sigfence` requires lvalues (documented) |
| `TSSD` (wontfix) | security | Low | `tss_async_signal_safe_destroy` double-destroy = unguarded UAF |
| `DEDE` (wontfix) | security | Low | `signal_decider_destroy` double-destroy = unguarded UAF |

*(wontfix) = adjudicated not to fix, per the C11/POSIX/N3924 contract, with the
rationale recorded in the heading body. 2026-08-16: `TSSD`/`DEDE` (double-destroy is
undefined behaviour; no guard will be added to a raw-pointer handle that `destroy()`
frees), and the *uncooperative* arm of the removed `JLGS` (a decider that never returns
without calling `sigdecider_abandon`). 2026-08-17: `UNTL`, `SPIN`, `ALOC`, `REEN`, `TAFL`,
`LEAK`, `DEIN`, `SIGF`, `CPPD`, `SFNK`, `FLGS`, `TAOM`, `FPUN`, `MAST`, `CRTS`, `MING`,
`FORK`, `NSIG`, `NDBS`, `UECL`, `CXAT`, `SFQL`, `DEDE`. 2026-08-18: `MLAS`, `SJMS`,
`PREI`, `WRET`, `SUST`, `SKIP`, `FWTF`, `SJSP`, `WXMS`, `CXXR`, `CIGA`, `TRAP`, `SFAR`.*
