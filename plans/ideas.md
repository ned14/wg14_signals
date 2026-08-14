# Ideas and techniques to adopt from `wg14_atomic_waits` — concretised

Review date: 2026-08-06 (concretised against the `wg14_signals` implementation source);
2026-08-14 status reconciliation (done items removed, the new analysis.md AB1 finding
folded in) and §1.3 implemented; 2026-08-14 §2.1 + §3.4 + §3.5 implemented;
2026-08-14 §2.2 (feature-test macro discipline) implemented; 2026-08-14 §2.3 (Windows
SDK floor) implemented; 2026-08-14 §2.5 (test `-Werror`, incl. analysis.md AA3)
implemented; 2026-08-14 §4.2 (`WG14_SIGNALS_STATIC_ASSERT` helper) implemented;
2026-08-14 §4.3 (Windows `sigset_t` bounds-checked bit shifts, fixing analysis.md 4.5)
implemented. Source reviewed: `../wg14_atomic_waits` at `8b40d4d` (HEAD), which was
derived from this project. Every idea is tied to a specific file and function in this
tree, shows the current code, and gives the concrete replacement (or a test design that
would have caught the defect). Sibling references are cited as
`../wg14_atomic_waits/include/...` / `.../CMakeLists.txt` and verified by direct reading.

Cross-references to `plans/analysis.md` findings use its stable IDs (e.g. "2.6", "V2",
"AB1"); `analysis.md` records verified reproductions inline.

---

## 1. Highest-impact adoptions (quick wins)

### 1.3 Per-test `TIMEOUT` (test hygiene) **[DONE 2026-08-14]**

**Concrete change.** `CMakeLists.txt` (`add_code_test`):

```cmake
function(add_code_test target)
  add_code_example(${target} ${ARGN})
  add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
  set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endfunction()
```

A hang is then reported against the specific test instead of only after the CI-wide
`ctest --timeout 300` expires.

**Done 2026-08-14:** `set_tests_properties(${target} PROPERTIES TIMEOUT 60)` added to
`add_code_test` in `CMakeLists.txt`; tests that need longer, or that bound a
pathological hang, keep their explicit larger `TIMEOUT` (the later
`set_tests_properties` calls in `test/CMakeLists.txt` override the default — verified:
`tss_concurrent_exit_test` 300, `siguninstall_raise_test` 300,
`post_uninstall_reentry_test` 300, `decider_mixed_set_test` 60,
`install_sighandler_lock_test` 60, `recovery_null_loop_test` 60, `sigfence_codegen_test`
300, `install_consumer_test` 180, `header_only_build_test` 600 all retain their values
while every other `add_code_test` test now reports 60). Full `ctest` suite (22 tests)
passes under ASan/UBSan on macOS arm64.

---

## 2. Build-system changes

### 2.1 Backend-override option (exercise the fallback TLS path everywhere) **[DONE 2026-08-14]**

**Why.** `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` is auto-detected in `config.h:41-51`;
Linux always takes the native TLS path, so the `tss_async_signal_safe` hash-table
fallback is only ever exercised on macOS (analysis.md 5.4). The sibling solves exactly
this class of problem with `ALWAYS_USE_PTHREADS_BACKEND` + generator-expression source
dispatch (`../wg14_atomic_waits/CMakeLists.txt:26-32`).

**Concrete change.** Add to `CMakeLists.txt`:

```cmake
option(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS "Force the tss_async_signal_safe hash-table TLS path on all platforms" OFF)
if(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS)
  target_compile_definitions(${PROJECT_NAME} PUBLIC WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0)
endif()
```

The PUBLIC define propagates to consumers so header-only consumers select the same path.
(The library source set is platform-agnostic — the posix vs windows `.ipp` selection is
done by `#ifdef _WIN32` in the headers — so no generator-expression source dispatch is
needed here.)

**Verification.** `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` on Linux exercises the fallback map, the
`thread_atexit` path, and the `sig_global_tss_state_*` functions (analysis.md 2.6) on the
platform with the strongest sanitizers.

**Done 2026-08-14:** the `option(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS ...)` and the PUBLIC
`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` compile definition were added to
`CMakeLists.txt` exactly as sketched. The define is PUBLIC, so it is baked into the
installed package's exported `INTERFACE_COMPILE_DEFINITIONS` — verified: with the option
ON the staged-install `wg14_signalsExports.cmake` carries
`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0`, so `find_package` consumers
(`install_consumer_test`) select the fallback path automatically. The header-only
consumers that do not link the library (`header_only_test`, `header_only_c_multi_test`,
and the single-TU C consumer driven by `header_only_build_test`) get the define
propagated from `test/CMakeLists.txt` / `test/header_only_c_consumer/CMakeLists.txt`
(the `header_only_build_test` step-1 and step-2 sub-builds forward the option).
**Verified:** `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` under ASan/UBSan passes the full `ctest`
suite (22 tests, including `header_only_test`, `header_only_c_multi_test`,
`header_only_build_test` and `install_consumer_test`), and the option OFF default build
is byte-for-byte unchanged; the define was confirmed present in the compile flags of the
library and all header-only targets. CI coverage is §3.4 (below).

### 2.2 Feature-test macro discipline **[DONE 2026-08-14]**

**Why.** `current_thread_id.c.ipp` hand-defines `_GNU_SOURCE` (`:20-22`); the white-box
tests and any header-only consumer must replicate feature-test macros by hand or
`clock_gettime`/`pthread_condattr_setclock` declarations are missing (analysis.md 4.6, X9
musl failure).

**Concrete change.** In `CMakeLists.txt` after `target_compile_definitions(... WG14_SIGNALS_SOURCE)` (`:31`):

```cmake
if(NOT MSVC)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _GNU_SOURCE)
endif()
```

Keep the mirrors in test/install_consumer/app.c and any white-box test (the sibling does
exactly this: `../wg14_atomic_waits/test/hash_table_whitebox_test.c:12-20`).

**Done 2026-08-14:** the library target now compiles with explicit feature-test macros
(PRIVATE) in `CMakeLists.txt` — selected by **escalating probe**, not by platform name.
Whether a libc/compile-mode exposes the symbols the library needs (NSIG — used
unguarded by the signo-to-sighandler map, analysis.md Y6 — the sigset functions, and
SIGSYS/SIGXCPU/SIGXFSZ, analysis.md 4.6) is a property of the toolchain, not of the OS
name. `CMakeLists.txt` therefore runs a `check_c_source_compiles` probe of a realistic
TU first with **no extra defines at all** (only the ones known to be mandatory on every
supported platform, i.e. none), and only if that fails adds defines one level at a time
until it compiles: `_GNU_SOURCE` → `_POSIX_C_SOURCE=200809L` →
`+_XOPEN_SOURCE=700` → the full trio. Most libcs already pass the empty level in their
default GNU-extension compile mode (verified on macOS arm64: level 0 passes and no
macros are added); strict-POSIX modes and libcs that ignore `_GNU_SOURCE` get exactly
the least intrusive set that works, and the full trio is only ever used where nothing
less works. `_GNU_SOURCE` alone is the usual non-empty winner because it subsumes the
trio on glibc/musl and is harmless on Apple/BSD. The mirrors were added to
`test/install_consumer/app.c` (which previously deliberately had *no* preamble and
documented why — that note is now updated) and the two white-box tests
(`install_sighandler_lock_test.c`, `signo_map_verstable_init_test.c`): each defines
`_GNU_SOURCE` with an `#ifndef` guard before the first system include, exactly the
sibling's `hash_table_whitebox_test.c` pattern — `_GNU_SOURCE` is the portable mirror
because it is either the selected set or a no-op/implied-superset on every platform the
library builds on. The per-file `#ifndef _GNU_SOURCE` hand-define in
`current_thread_id.c.ipp` stays as the header-only-consumer fallback. **Verified:**
default and `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` builds under ASan/UBSan on macOS
arm64 pass the full `ctest` suite (22 tests), including `install_consumer_test` and both
white-box tests; the macOS configure log shows the level-0 probe passing and no macros
being added; the escalation control flow (non-empty level selection and the no-match
warning) was verified with a standalone CMake simulation. The non-empty ladder levels
are exercised by the Linux CI legs (glibc strict mode picks `_GNU_SOURCE`).

### 2.3 Windows SDK floor **[DONE 2026-08-14]**

**Concrete change.** The Windows backend uses `AddVectoredContinueHandler` /
`SetUnhandledExceptionFilter` (available since XP) and `RaiseException`; decide the floor
and enforce it both in CMake and in the `.ipp`, mirroring
`../wg14_atomic_waits/CMakeLists.txt:57-60` and `atomic_wait_windows.c.ipp:26-29`:

```cmake
if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PUBLIC _WIN32_WINNT=0x0600 WINVER=0x0600)
endif()
```

and at the top of `thrd_signal_handle_windows.c.ipp`:
```c
#if defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
#error "wg14_signals requires _WIN32_WINNT >= 0x0600"
#endif
```

**Done 2026-08-14:** the floor was set at 0x0600 (Windows Vista). `CMakeLists.txt`
gains the `if(WIN32)` block defining `_WIN32_WINNT=0x0600 WINVER=0x0600` **PUBLIC** (so
consumers, the installed package's exported target, and header-only compilations of the
`.ipp` implementations all get the same declarations), placed after the feature-test
macro block; `thrd_signal_handle_windows.c.ipp` gains the guard immediately after its
include guard, before the header include. The guard deliberately fires only when
`_WIN32_WINNT` is *defined and too low* (not when undefined): the library build always
defines it at 0x0600, and when undefined the Windows SDK default (>= 0x0600 on any
modern SDK) applies, so header-only consumers that never set `_WIN32_WINNT` are not
rejected. The chosen floor covers every API the backend uses (`AddVectoredContinueHandler`,
`SetUnhandledExceptionFilter`, `RaiseException`, and the `IMAGE_TLS_DIRECTORY` mechanism
in thread_atexit() — all Vista-and-earlier), and matches the sibling's structure.
**Verified:** macOS arm64 build is unaffected (the block is `WIN32`-gated) and the full
`ctest` suite (22 tests) passes; the guard's three scenarios (0x0501 -> `#error`,
0x0600 -> ok, undefined -> ok) were verified with a standalone preprocessor probe; the
Windows CI legs build the library with `_WIN32_WINNT=0x0600` defined PUBLIC.

### 2.5 Test `-Werror` (fixes analysis.md 5.3) **[DONE 2026-08-14]**

**Why.** Library builds with `-Werror`; tests with only `-Wall -Wextra -Wpedantic`, so
test-only warnings are invisible.

**Concrete change.** In `test/CMakeLists.txt:10-13` change the non-MSVC branch to
`-Wall -Wextra -Wpedantic -Werror` (the header-only test needs a
`WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`-guard first — analysis.md AA3 — since `sigfence(...)`
is exercised in tests and its `__VA_OPT__` counting trips `-Wpedantic`). Add
compile-fail tests (§6.7) so intended diagnostics are proven rather than just warned
about.

**Done 2026-08-14:** `-Werror` is now applied to every test target's non-MSVC branch:
`add_code_example` in `CMakeLists.txt` (covers all `add_code_test` targets including the
benchmarks), the `header_only_test` and `header_only_c_multi_test` targets in
`test/CMakeLists.txt`, and the install-consumer's `install_consumer_app` in
`test/install_consumer/CMakeLists.txt` (the consumer's job is to prove the installed
package compiles cleanly). The required AA3 prerequisite was done first: the
sigfence-dependent tests are now guarded by `#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`
(analysis.md 6.6, fixed), and the zero-arg `sigfence()` overload's `-Wpedantic`
diagnostic (`-Wvariadic-macro-arguments-omitted` on clang — which `sigfence_fence_test.c`
already suppressed — plus the gcc equivalent `-Wvariadic-macros`) is suppressed around
exactly that call. The §6.7 compile-fail suite remains a separate item. **Verified:**
clean rebuild with `-Werror` produces zero warnings on macOS arm64 (library, all 20 test
executables, both header-only targets, and the install consumer), the full `ctest` suite
(22 tests) passes, and both sigfence tests compile with the knob defined under `-Werror`.
The gcc legs of the Linux CI exercise the gcc-specific warning set.

---

## 3. CI configuration (`.github/workflows/ci.yml`)

`wg14_signals` runs 3 jobs; the sibling runs 7. The porting notes below are concrete YAML
adapted to this repo (branch `main`, no `ALWAYS_USE_PTHREADS` dimension, tests use
`thrd_*`).

### 3.4 Fallback-TLS matrix dimension **[DONE 2026-08-14]**

After §2.1, add `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=OFF/ON` to the Linux and macOS matrix — the
analogue of the sibling's `pthreads` dimension. This is what puts the fallback path under
ASan/UBSan on Linux, where the strongest tooling is.

**Done 2026-08-14:** the `fallback: [OFF, ON]` matrix dimension was added to the Linux
job (now 32 legs) and the MacOS job (now 16 legs) in `.github/workflows/ci.yml`, with
`-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=${{ matrix.fallback }}` passed to both configure steps and the
matrix leg included in each job's `NAME`. The job names follow the sibling's convention
(`../wg14_atomic_waits/.github/workflows/ci.yml`: `Linux (clang, Debug, C11, shared=OFF,
fallback=ON)`), with the `NAME` env using the `-C<standard>`/`shared=`/`fallback=`
prefixes. Linux is where the option actually changes behaviour (config.h autodetects the
native TLS path there, so the ON legs run the `tss_async_signal_safe` hash-table fallback
under ASan/UBSan with both clang and gcc); on macOS the fallback is already the native
autodetected path, so its ON legs re-verify the option plumbing only. The FreeBSD leg's
comment was updated (it no longer describes the fallback as "exercised only by the macOS
legs"). Windows is deliberately not given the dimension: forcing the fallback there
exercises `tss_async_signal_safe` under MSVC's `/experimental:c11atomics` (see §8).

### 3.5 Matrix hygiene (trivial) **[DONE 2026-08-14]**

Add `fail-fast: false` to the Linux job (`ci.yml:16` — macOS and Windows jobs already
have it); `-E benchmark` is already excluded everywhere; ensure the Windows job keeps
`--config`/`-C` for multi-config generators (it does).

**Done 2026-08-14:** the Linux job already carries `fail-fast: false`, `-E benchmark`
was already excluded on every leg, and the Windows job already passes
`--config`/`-C` — no CI change was required; §3.4's matrix edits preserved all three.

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

### 4.2 `_WG14_SIGNALS_STATIC_ASSERT` helper **[DONE 2026-08-14]**

**Concrete change.** Add to `config.h` (port of `../wg14_atomic_waits/atomic_wait.h:146-150`):

```c
#ifndef WG14_SIGNALS_STATIC_ASSERT
#ifdef __cplusplus
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif
#endif
```

Use it (with the sibling's literal, namespaced message text) wherever a compile-time
guarantee belongs — e.g. a `sizeof(sigset_t) <= UINT32_MAX` check for the Windows
`sigset_t` redefinition (`thrd_signal_handle.h:43-63`, see 4.3) and for the
`WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` `__VA_OPT__` requirement (`thrd_signal_handle.h:85-86`).
These become the targets of the compile-fail suite (§6.7).

**Done 2026-08-14:** the helper `WG14_SIGNALS_STATIC_ASSERT(cond, msg)` (C11
`_Static_assert` / C++11 `static_assert`, user-overridable via `#ifndef`) was added to
`config.h`, and both suggested uses were added to `thrd_signal_handle.h`:
- The Windows `sigset_t` block asserts `sizeof(sigset_t) >= sizeof(uint32_t)` — the
  32-signal bit-set scheme (`sigfillset == UINT32_MAX`, shifts 1..32, plans/ideas.md 4.3)
  requires the redefinition to stay at least 32 bits wide. (The plan's literal
  `sizeof(sigset_t) <= UINT32_MAX` condition is dimensionally vacuous — `sizeof` in bytes
  compared to the max value — so it was interpreted as the meaningful lower-bound check.)
- The sigfence macro block asserts the `__VA_OPT__`-based counting machinery returns the
  right counts: `WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(a,b,c) == 3 && ...(a,b,c,d,e) == 5`.
  On a compiler without `__VA_OPT__` the expansion is a hard preprocessing error; on a
  compiler with broken counting the assertion fails. **Deviation:** the zero-argument
  path is deliberately *not* asserted in the header — calling the variadic macro with an
  empty argument list triggers clang/gcc's `-Wvariadic-macro-arguments-omitted` /
  `-Wvariadic-macros` `-Wpedantic` diagnostic, and a public header must not ship a pragma
  suppression for it; the zero-arg path stays exercised by `test/sigfence_fence_test.c`
  (whose TU suppresses the diagnostic). **Verified:** the header compiles cleanly under
  `-std=c11` and C++11 with `-Wall -Wextra -Wpedantic -Werror`; the helper fails loudly on
  a false condition (`static assertion failed ... wg14_signals: ...`) and passes on true
  conditions in both C and C++; the full `ctest` suite (22 tests) passes. The two asserts
  are the natural targets for the §6.7 compile-fail suite.

### 4.3 Windows `sigset_t`: bound-check the bit shifts **[DONE 2026-08-14]**

**Why.** `thrd_signal_handle.h:52-63` compute `1u << (signo - 1)` with no range check;
`signo > 32` is UB (analysis.md 4.5), and `sigaddset`/`sigismember` are also called from
signal-handler context.

**Concrete change.** Add the bounds check to all four helpers, e.g.:

```c
static inline void sigaddset(sigset_t *ss, const int signo)
{
  if(signo >= 1 && signo <= 32)
    *ss |= (1u << (signo - 1));
}
```

and keep `sigismember` total (return false out of range) so the Windows `sigfillset_*`
lazy checks don't read a torn set.

**Done 2026-08-14:** the three shifting helpers (`sigaddset`, `sigdelset`,
`sigismember`; `sigemptyset`/`sigfillset` don't shift) now bounds-check `signo` against
`[1, 32]` in `thrd_signal_handle.h`. `sigaddset`/`sigdelset` are no-ops out of range;
`sigismember` short-circuits to `false` out of range, keeping it total so the Windows
`sigfillset_*` lazy-init checks (`sigismember(&v, signos[0])`) never read a torn set. The
helpers run in signal-handler context, so the choice is silent no-op / total false rather
than error reporting. The `32` bound is backed by the §4.2 static assert (the set is at
least `uint32_t`-wide). **Verified:** a standalone ASan/UBSan probe exercising signo
{-1, 0, 1, 2, 15, 31, 32, 33, 64} confirmed out-of-range adds/dels are no-ops,
`sigismember` is false out of range with no shift UB, and in-range behaviour is
unchanged; the full `ctest` suite (22 tests) passes.

### 4.4 Backend `.ipp` include guards + platform `#error` guards [partially DONE]

**Why.** `thrd_signal_handle_posix.c.ipp` / `_windows.c.ipp` / the shared
`tss_async_signal_safe.c.ipp` had no include guards of their own; double inclusion was
prevented only by the parent headers' guards and the `#ifdef _WIN32` dispatch in
`thrd_signal_handle.h:463-469`. The sibling adds per-file guards *and* a `#error` guard so
a wrong-platform include is a clear compile error instead of a silent mis-compile
(`../wg14_atomic_waits/atomic_wait_macos.c.ipp:20-25`).

**Concrete change.** Include guards were added in the header-only fix (2026-08-09); still
to do — add the wrong-platform `#error` to the POSIX file:

```c
#ifdef _WIN32
#error "thrd_signal_handle_posix.c.ipp must only be included on non-Windows"
#endif
```

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
under `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON` (§2.1).

### 5.3 `tss_async_signal_safe_thread_init`: re-check under the lock (fixes analysis.md 3.13, 3.14, 2.5)

**Why.** `:184-200`: the user `create` callback runs outside `mem->lock`, then the map is
inserted without re-checking. Two racing threads both run `create` (leak of the loser's
value, double `count`, double atexit registration — 3.13); a `create` returning 0 with
NULL leaves `errno`/state inconsistent (2.5); a failed `thread_atexit` leaves a committed
entry and count (3.14).

**Concrete change.** Port the sibling's "capture/check under the lock" idiom
(`../wg14_atomic_waits/atomic_wait_common.ipp.ipp:556-589`): run `create` unlocked, then
re-lock, re-`get` the map, and if another thread won the race discard the copy (call
`mem->attr.destroy(newitem)`); treat `ret == 0 && newitem == NULL` as failure (`-1`,
`errno = EINVAL`); register `thread_atexit` LAST, and if it fails roll back the insert and
count before returning -1.

### 5.4 `tss_async_signal_safe_get`: lock-free read via a per-thread cached pointer

**Why.** `get` (`:226-243`) takes `mem->lock`, which is also taken inside signal-handler
context (`sig_global_tss_state` -> `tss_async_signal_safe_get`). A spinlock in a handler
deadlocks if the interrupted thread holds it (3.1) — so the "ASYNC-SIGNAL-SAFE" claim
(§7 of analysis.md) is false in general. The sibling's `current_thread_id()` solves
exactly this by caching in TLS and only falling back to a slow path on cache miss
(`../wg14_atomic_waits/current_thread_id.h:58-72`); `tss_async_signal_safe.c.ipp` already
does the same for the TID (`my_current_thread_id`, `:81-91`).

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
header (already partially documented at `thrd_signal_handle.h:408-412`).

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

### 5.8 Windows backend concrete fix (analysis.md V2)

- **NULL `tss->front` deref from a fresh thread (V2)** — the vectored handler calls
  `sig_global_tss_state()` on threads that never ran `sig_global_tss_state_init`; on the
  fallback path that returns NULL and `tss->front` dereferences it. Guard:

  ```c
  struct ... *tss = WG14_SIGNALS_PREFIX(sig_global_tss_state)();
  if(tss != WG14_SIGNALS_NULLPTR && tss->front != WG14_SIGNALS_NULLPTR)
  {
    longjmp(tss->front->buf, 1);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
  ```

### 5.9 `siguninstall`/`signal_decider_destroy` locking hygiene

- **`signal_decider_destroy` takes `state->lock` once per signal** (analysis.md §9):
  `:564-611` loops `LOCK/UNLOCK` around each signal's map access. Acquire once before the
  loop and release after.
- **`siguninstall` `-1` failure path leaks `ss`** (analysis.md §9): `:423-426`
  `return -1` before `free(ss)`; free first. (`uninstall_sighandler` always returns true so
  the path is currently dead, but keep it correct.)
- **`signal_decider_destroy` leaks the node on the normal destroy path (analysis.md
  AB1):** the AA1 fix left the post-unlock free as a *second* refcount decrement, so a
  node whose first decrement hit 0 (still-installed signal, base refcount 1) is
  LIST_REMOVE'd and then counted down to -1 and never freed. Concrete fix: free the node
  where its refcount first reaches zero (in the map-entry branch, after the
  `sighandler_info_has_decider` walk), and leave the post-unlock block for the map-miss
  path only — i.e. restructure the second decrement as `else` on "the map had no entry",
  or simply restore `free(*retp)` in the map-entry refcount-zero branch.

---

## 6. Testing techniques (concrete designs)

### 6.1 `test_wait_until`: bounded spin handshake (anti-flake)

**Concrete change.** Port `../wg14_atomic_waits/test/test_common.h:109-138` into
`test/test_common.h` verbatim (adapted to `WG14_SIGNALS_*` names): a
`test_wait_until(const char *what, const atomic_int *value, int goal)` that spins with
`timespec_get` deadline and `abort()`s after 2000 ms with a named diagnostic. Rewrite the
`while(atomic_load(...) == 2) {}` handshakes in `thrd_signal_handle_test.c:118-120` and
`thrd_sigfpe_test.c` to use it. (AGENTS.md rule 5.)

### 6.3 `SECTION(...)` progress markers

**Concrete change.** Add `#define SECTION(name) fprintf(stderr, "<test>: " name "\n")` to
`test_common.h` and emit one before each phase of `thrd_signal_handle_test.c` (three
`puts(...)` already exist — convert them), `thrd_sigfpe_test.c`, and
`async_signal_safe_tls_test.c`. ctest echoes stderr only on failure, so a hang becomes
localisable.

### 6.4 Remaining regression tests to add (each one maps to a verified analysis finding)

Add to `test/` (all `add_code_test`, C11):

| Test file | Exercise | Catches |
|---|---|---|
| `tss_null_handle_test.c` | `create/destroy/thread_init/get` on NULL and zeroed handles | 2.6 |
| `lock_whitebox_test.c` | `#include "detail/impl/lock_unlock.h"`, lock/unlock under TSan | 3.1 discipline |
| `decider_destroy_leak_test.c` | loop 10,000 x {create, destroy} on an installed signal; assert no node allocation growth (malloc/calloc interposition, the analysis.md AB1 pattern) | AB1 |

### 6.5 White-box tests (include internals directly)

The sibling's `hash_table_whitebox_test.c` includes the backend `.ipp` with
`WG14_ATOMIC_WAITS_ENABLE_HEADER_ONLY` and drives internals single-threaded. For
`wg14_signals`:

- `lock_whitebox_test.c` (above) drives `LOCK/UNLOCK` directly — a white-box test that
  calls `signal_decider_create` with a mixed set and then verifies a fresh
  `LOCK(state->lock)` succeeds would have caught the original lock leak in minutes.
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
- `windows_sigset_overflow`: after the §4.3 bounds check `sigaddset(&ss, 33)` becomes a
  no-op rather than a compile error, so instead make this a
  `WG14_SIGNALS_STATIC_ASSERT`-backed compile-fail for the `sizeof(sigset_t) <= 4` contract
  (4.2) on Windows.

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
  `<pthread.h>`); their docs still claim "every platform". For the §2.1
  `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS` option there is no such problem (it is pure
  compile-definition), but note that forcing the fallback on Windows exercises
  `tss_async_signal_safe` with MSVC's `/experimental:c11atomics`.
- **The hash-table proxy engine is tuned for wait/notify**; adopt the *techniques*
  (§5.2-5.4: re-check-under-lock, refcounted containers, lock-free cached reads) rather
  than the structure wholesale. Replacing `verstable.h` with a triangular-probing table is
  a larger refactor; the concrete bug that motivated it (analysis.md Z1: uninitialised
  verstable map on NSIG>=1024) was fixed exactly as recommended here — initialising the
  map instance in `sig_global_state()` — so there is no longer a defect pushing the
  replacement.
- **CI breadth**: 7 jobs/~80 legs in the sibling is proportional to its problem; for
  `wg14_signals` defer the fallback-TLS matrix breadth (§3.4) until the §1 quick wins
  land.

---

## 9. Priority-ordered adoption plan

| # | Change | Location | Fixes (analysis.md) | Effort |
|---|--------|----------|--------------------|--------|
| 1 | Fix `signal_decider_destroy` node leak: free at the refcount-zero branch, keep the map-miss block for uninstalled signals only | `thrd_signal_handle_common.ipp.ipp:708-755` | AB1 | Small |
| 2 | Fallback-path setup: NULL-safe tss API | `tss_async_signal_safe.c.ipp:93-243` | 2.6 | Small |
| 3 | Windows NULL-tss guard (V2) | `thrd_signal_handle_windows.c.ipp:357-367` | V2 | Small |
| 4 | Per-test `TIMEOUT 60` **[DONE 2026-08-14]** | `CMakeLists.txt:88-91` | test hygiene | Trivial |
| 5 | `tss_async_signal_safe`: lock-free `get` via cached TLS value; init re-check | `tss_async_signal_safe.c.ipp:136-243` | 3.1, 3.13, 3.14, 2.5 | Medium |
| 6 | `install_sighandler` flags: drop `SA_NOCLDWAIT`, add `SA_RESTART` | `thrd_signal_handle_posix.c.ipp:371-383` | 3.3 | Small |
| 7 | Remaining regression tests (tss NULL, lock whitebox, decider-destroy leak) | `test/*` | 2.6, 3.1, AB1 | Medium |
| 8 | Compile-fail suite (`expect_compile_fail.cmake` + sigfence targets) | `test/` | 5.3, 2.8, X11 | Medium |
| 9 | `sigfillset_*` constructor-attribute removal + init under lock | `thrd_signal_handle_posix.c.ipp:49-127` | 7.1, C11 rule 1 | Small |
| 10 | MSVC c11-atomics helper (feature-test macros: **[DONE 2026-08-14]**) | `CMakeLists.txt` | 4.6, X9 | Small |
| 11 | `docs/proposal.md` + `plans/test-review-todos.md` + `.gitattributes` trim | `docs/`, `plans/`, `.gitattributes` | process | Small |
