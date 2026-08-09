# Ideas and techniques to adopt from `wg14_atomic_waits` — concretised

Review dates: 2026-08-06 (initial) and 2026-08-06 (concretised against the
`wg14_signals` implementation source).

Source reviewed: `../wg14_atomic_waits` at `8b40d4d` (HEAD), which was derived
from this project (`wg14_signals`). This revision of the document was written
against the actual `wg14_signals` code: every idea below is tied to a specific
file and function in this tree, shows the current code, and gives the concrete
replacement (or a test design that would have caught the defect). Sibling
references are cited as `../wg14_atomic_waits/include/...` / `.../CMakeLists.txt`
and verified by direct reading.

Cross-references to `plans/analysis.md` findings (e.g. "1.1", "V1", "Y1") are
the existing bug dossier; `analysis.md` §10/§12-17 record verified
reproductions.

---

## 1. Highest-impact adoptions (quick wins)

### 1.1 Packaging: make the installed package usable (fixes analysis.md V1)

**Why.** `find_package(wg14_signals REQUIRED)` currently hard-fails and the
installed tree ships no headers (verified, V1). Two root causes in
`CMakeLists.txt`:

- `:58-62` uses `configure_file(... @ONLY)`, so `@PACKAGE_INIT@` and
  `check_required_components()` in `cmake/ProjectConfig.cmake.in` are never
  expanded → `Unknown CMake command "check_required_components"`.
- There is no `install(DIRECTORY include/...)`, so nothing under
  `<wg14_signals/...>` exists after install.

**Concrete change.** In `CMakeLists.txt`, replace the `configure_file` block
(`:58-62`) with:

```cmake
include(CMakePackageConfigHelpers)   # add near line 11
...
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/ProjectConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
install(DIRECTORY include/wg14_signals
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
```

and change line 10 to `project(wg14_signals VERSION 1.0.0 LANGUAGES C CXX)`.
(The `install(TARGETS ... EXPORT ...)` at `:50-56` and the `install(EXPORT ...)`
at `:67-70` stay as-is; `@PROJECT_NAME@Exports.cmake` becomes real and
`check_required_components` now exists.) This is the outward face of the
library and today it is unusable.

### 1.2 Add an install-consumer ctest (regression-proofs 1.1)

**Concrete change.** Copy the sibling's `test/install_consumer/` pattern
(`../wg14_atomic_waits/test/install_consumer/`, 141-line `driver.cmake` +
`app.c` + `CMakeLists.txt`) and register it in `test/CMakeLists.txt`:

```cmake
add_test(
  NAME install_consumer_test
  COMMAND ${CMAKE_COMMAND}
    -DINSTALL_TEST_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DINSTALL_TEST_CONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install_consumer
    -DINSTALL_TEST_PREFIX=${CMAKE_BINARY_DIR}/install_test_prefix
    -DINSTALL_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/install_test_consumer
    -DINSTALL_TEST_VERSION=${PROJECT_VERSION}
    -DINSTALL_TEST_CONFIG=$<CONFIG>
    -DINSTALL_TEST_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}
    -DINSTALL_TEST_C_COMPILER=${CMAKE_C_COMPILER}
    -DINSTALL_TEST_C_FLAGS=${CMAKE_C_FLAGS}
    -DINSTALL_TEST_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
    -DINSTALL_TEST_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/install_consumer/driver.cmake
)
set_tests_properties(install_consumer_test PROPERTIES TIMEOUT 180)
```

The consumer `app.c` (thread-free; its job is packaging, not concurrency) must
mirror the sibling's "feature-test macros first" preamble (glibc needs
`_POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _GNU_SOURCE` before the first
include — see `../wg14_atomic_waits/test/install_consumer/app.c:11-19`) and
then exercise: `stdc_raise(0, NULL, NULL)` setup, `sigfillset_synchronous`,
`siginstall(NULL)` + a `signal_decider_create`/`stdc_raise`/destroy cycle, and
`sigfence(x)`. The driver asserts the installed headers / Config / ConfigVersion
/ Exports files exist, then configures + builds + runs the consumer with the
parent toolchain (so ASan/UBSan/TSan/Fil-C legs still link) and the per-OS
loader-path handling (`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, and the Windows
`PATH` semicolon-escaping gotcha at `driver.cmake:122-136`).

**Verification.** `ctest -R install_consumer_test` fails before the 1.1 fix
("missing include/wg14_signals/...") and passes after.

### 1.3 Header-only mode: `static inline` instead of `inline` (fixes analysis.md 1.8, C3, Y8, Y10)

**Why.** `config.h:109-115` defines `WG14_SIGNALS_EXTERN` as plain `WG14_SIGNALS_INLINE`
(`inline`) in header-only mode. Consequences, all verified:

- `-Werror,-Wstatic-local-in-inline`: `sig_global_state()`
  (`thrd_signal_handle_common.ipp.ipp:174-179`) is `WG14_SIGNALS_EXTERN` (external
  `inline`) with a function-local `static`.
- `-Werror,-Wstatic-in-inline`: public `inline` functions call `static` helpers
  (e.g. the `sigfillset_*` sets, `prepare_rsi`, `install_sighandler`).
- C11 6.7.4p7: with every file-scope declaration `inline`, the .ipp
  definitions emit no external symbol → C header-only consumers get *undefined
  symbols* (C3, Y10), and `thread_atexit()` has no C definition at all
  (`thread_atexit.h:41`).
- C++ multi-TU: duplicate-symbol/linkage failure (Y8).

**Concrete change.** `config.h:111`:

```c
#ifndef WG14_SIGNALS_EXTERN
#if WG14_SIGNALS_ENABLE_HEADER_ONLY
// Per-TU static inline: an external-linkage inline function may not call the
// internal static helpers used by the shared implementation
// (-Wstatic-in-inline), and per-TU static definitions avoid duplicate symbols
// when the header is included from more than one translation unit.
#define WG14_SIGNALS_EXTERN static WG14_SIGNALS_INLINE
#else
#define WG14_SIGNALS_EXTERN WG14_SIGNALS_EXTERN_IMPL
#endif
#endif
```

**Verification.** `-DHEADER_ONLY_BUILD=ON` builds (it currently does not, 1.8);
a *C* header-only test TU links at `-O0` (it currently does not, C3); a 3-TU C++
header-only program links (Y8/Y10).

### 1.4 Per-test `TIMEOUT` (test hygiene)

**Concrete change.** `CMakeLists.txt:88-91` (`add_code_test`):

```cmake
function(add_code_test target)
  add_code_example(${target} ${ARGN})
  add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
  set_tests_properties(${target} PROPERTIES TIMEOUT 60)
endfunction()
```

A hang is then reported against the specific test instead of only after the
CI-wide `ctest --timeout 300` expires.

### 1.5 AGENTS.md rules (developer discipline)

Port rules 4 and 5 from `../wg14_atomic_waits/AGENTS.md`:

- **Rule 4**: C++ is permitted in `test/` *solely* for compile-testing the
  public header and `extern "C"` linkage; never under `include/` or `src/`.
  This targets the `thread_atexit.cpp` problem class (analysis.md 5.2, Y7,
  AA7): the one C++ file in `src/` forces a C++ toolchain and drags a hidden
  C++ runtime dependency into every C consumer of the static library.
- **Rule 5**: "Never, EVER use sleeps alone to synchronise between threads...
  ALWAYS use a proper synchronisation between threads." (Backbone of §6.1.)

---

## 2. Build-system changes

### 2.1 Backend-override option (exercise the fallback TLS path everywhere)

**Why.** `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` is auto-detected in
`config.h:41-51`; Linux always takes the native TLS path, so the
`tss_async_signal_safe` hash-table fallback is only ever exercised on macOS
(analysis.md 5.4). The sibling solves exactly this class of problem with
`ALWAYS_USE_PTHREADS_BACKEND` + generator-expression source dispatch
(`../wg14_atomic_waits/CMakeLists.txt:26-32`).

**Concrete change.** Add to `CMakeLists.txt`:

```cmake
option(ALWAYS_USE_FALLBACK_TLS "Force the tss_async_signal_safe hash-table TLS path on all platforms" OFF)
if(ALWAYS_USE_FALLBACK_TLS)
  target_compile_definitions(${PROJECT_NAME} PUBLIC WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0)
endif()
```

The PUBLIC define propagates to consumers so header-only consumers select the
same path. (The library source set itself is platform-agnostic — the posix vs
windows `.ipp` selection is done by `#ifdef _WIN32` in the headers — so no
generator-expression source dispatch is needed here, unlike the sibling.)

**Verification.** `-DALWAYS_USE_FALLBACK_TLS=ON` on Linux now exercises the
fallback map, the `thread_atexit` path, and the `sig_global_tss_state_*`
functions (fixes 1.3/2.4/2.6 family) on the platform with the strongest
sanitizers.

### 2.2 MSVC C11-atomics helper function

**Why.** `CMakeLists.txt:45` (`/experimental:c11atomics` for MSVC) is applied
only to the library target; `test/CMakeLists.txt:10` re-applies it only to the
header-only test. New targets silently miss it on MSVC.

**Concrete change.** Add near `CMakeLists.txt:12` and call from the library and
every `add_code_example`/test target (port of
`../wg14_atomic_waits/CMakeLists.txt:20-24`):

```cmake
function(wg14_signals_enable_c11_atomics target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /experimental:c11atomics)
  endif()
endfunction()
```

### 2.3 Feature-test macro discipline

**Why.** `current_thread_id.c.ipp` hand-defines `_GNU_SOURCE` (`:20-22`); the
white-box tests and any header-only consumer must replicate feature-test macros
by hand or `clock_gettime`/`pthread_condattr_setclock` declarations are
missing (analysis.md 4.6, X9 musl failure).

**Concrete change.** In `CMakeLists.txt` after `target_compile_definitions(... WG14_SIGNALS_SOURCE)` (`:31`):

```cmake
if(NOT MSVC)
  target_compile_definitions(${PROJECT_NAME} PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _GNU_SOURCE)
endif()
```

Keep the mirrors in test/install_consumer/app.c and any white-box test (the
sibling does exactly this: `../wg14_atomic_waits/test/hash_table_whitebox_test.c:12-20`).

### 2.4 Windows SDK floor

**Concrete change.** The Windows backend uses `AddVectoredContinueHandler` /
`SetUnhandledExceptionFilter` (available since XP) and `RaiseException`; decide
the floor and enforce it both in CMake and in the `.ipp`, mirroring
`../wg14_atomic_waits/CMakeLists.txt:57-60` and
`atomic_wait_windows.c.ipp:26-29`:

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

### 2.5 FreeBSD `stdthreads` link (prerequisite for §3.4)

**Concrete change.** In `CMakeLists.txt` `add_code_example` (near `:82`):

```cmake
target_link_libraries(${target} PRIVATE $<$<PLATFORM_ID:FreeBSD>:stdthreads>)
```

The C11 `thrd_*` tests need it on FreeBSD
(`../wg14_atomic_waits/CMakeLists.txt:135`).

### 2.6 Test `-Werror` (fixes analysis.md 5.3)

**Why.** Library builds with `-Werror`; tests with only
`-Wall -Wextra -Wpedantic`, so test-only warnings are invisible.

**Concrete change.** In `test/CMakeLists.txt:10-13` change the non-MSVC branch
to `-Wall -Wextra -Wpedantic -Werror` (the header-only test needs a
`WG14_SIGNALS_DISABLE_SIGFENCE_MACRO`-guard first — analysis.md AA3 — since
`sigfence(...)` is exercised in tests and its `__VA_OPT__` counting trips
`-Wpedantic`). Add compile-fail tests (§6.7) so intended diagnostics are
proven rather than just warned about.

---

## 3. CI configuration (`.github/workflows/ci.yml`)

`wg14_signals` runs 3 jobs; the sibling runs 7. The porting notes below are
concrete YAML adapted to this repo (branch `main`, no `ALWAYS_USE_PTHREADS`
dimension, tests use `thrd_*`).

### 3.1 HeaderOnly job (fixes analysis.md 1.8 / 5.4)

Append a job mirroring `../wg14_atomic_waits/.github/workflows/ci.yml:146-218`,
matrix over `{os: ubuntu-latest, macos-latest, windows-latest} × standard`
(C11/C23 POSIX, C11/C17 Windows), configure with
`-DHEADER_ONLY_BUILD=ON` plus the sanitize toolchain (POSIX) or
`-DCMAKE_C_FLAGS=/fsanitize:address` (Windows), build and
`ctest --output-on-failure --timeout 300 -E benchmark`. 6 legs suffice (no
pthreads dimension yet). This job proves the §1.3 fix on every runner.

### 3.2 TSan job (fixes analysis.md 5.4 "no TSan"; targets 2.1, 2.2, 3.1)

Append the sibling's TSan job (`ci.yml:220-277`): Ubuntu gcc+clang × C11/C23
and macOS clang × C11/C23 (6 legs), `-DCMAKE_TOOLCHAIN_FILE=$PWD/../cmake/tsan-toolchain.cmake`, with

```yaml
env:
  TSAN_OPTIONS: halt_on_error=1 log_path=stderr symbolize=1 history_size=7
steps:
  - name: Allow TSan to start (lower ASLR entropy, Linux only)
    if: runner.os == 'Linux'
    run: sudo sysctl vm.mmap_rnd_bits=28 || true
```

`cmake/tsan-toolchain.cmake` must be created (copy
`../wg14_atomic_waits/cmake/tsan-toolchain.cmake`: `-fsanitize=thread` on
C/CXX and linker flags). **Before this job can pass, port §6.2**
(TSAN-aware `<threads.h>` fallback): glibc's `thrd_create()` calls
`pthread_create()` inside libc, bypassing TSan's interceptor, and the spawned
thread crashes immediately (verified in the sibling suite).

### 3.3 FreeBSD VM job (fixes analysis.md 5.4, 4.1)

Append the sibling's FreeBSD job (`ci.yml:104-144`):
`vmactions/freebsd-vm@v1` (real FreeBSD 15 kernel, `cache-after-prepare: true`),
`prepare: pkg install -y cmake ninja`, Ninja generator, sanitize toolchain,
Release, C11/C23. **Directly relevant**: `current_thread_id.c.ipp:71` calls
`pthread_getthreadid_np()` — the only place that branch is compiled is
FreeBSD. Pair it with the §2.5 `stdthreads` link.

### 3.4 FilC job + toolchain fix (fixes analysis.md AA2)

**Why.** `cmake/filc-toolchain.cmake` is broken: hardcoded
`/home/ned/Downloads/filc-0.668.2-linux-x86_64/...` paths, and its
`-DDISABLE_INLINE_ASM=1` is a no-op for the library (only `test/ticks_clock.h`
honours it — AA2).

**Concrete change.** Rewrite `cmake/filc-toolchain.cmake` (port of
`../wg14_atomic_waits/cmake/filc-toolchain.cmake`):

```cmake
if(NOT FILC_ROOT)
  if(DEFINED ENV{FILC_ROOT})
    set(FILC_ROOT "$ENV{FILC_ROOT}")
  else()
    message(FATAL_ERROR "filc-toolchain.cmake: set FILC_ROOT or -DFILC_ROOT=<path>")
  endif()
endif()
set(CMAKE_C_COMPILER "${FILC_ROOT}/bin/clang")
set(CMAKE_CXX_COMPILER "${FILC_ROOT}/bin/clang++")
set(CMAKE_C_FLAGS "-D__FILC__=1 -DDISABLE_INLINE_ASM=1")
set(CMAKE_CXX_FLAGS "-D__FILC__=1 -DDISABLE_INLINE_ASM=1")
```

and append the sibling's download/verify job (`ci.yml:279-320`): pin
`FILC_VERSION` + `FILC_SHA256`, `curl` + `sha256sum -c`, `setup.sh`,
`echo "FILC_ROOT=..." >> $GITHUB_ENV`. To make `DISABLE_INLINE_ASM` meaningful
for the library itself, gate the GNU `SIGFENCE_IMPL_*` inline-asm block in
`thrd_signal_handle.h:97-131` on `#ifndef DISABLE_INLINE_ASM` (falling back to
the `sigfence_force_escaped` path) — that is the real AA2 fix.

### 3.5 Fallback-TLS matrix dimension

After §2.1, add `-DALWAYS_USE_FALLBACK_TLS=OFF/ON` to the Linux and macOS
matrix — the analogue of the sibling's `pthreads` dimension. This is what puts
the fallback path under ASan/UBSan on Linux, where the strongest tooling is.

### 3.6 Matrix hygiene (trivial)

Add `fail-fast: false` to the Linux job (`ci.yml:16` — macOS and Windows jobs
already have it) and `-E benchmark` is already excluded everywhere; ensure the
Windows job keeps `--config`/`-C` for multi-config generators (it does).

---

## 4. Header and macro techniques

### 4.1 `WG14_SIGNALS_ATOMIC_PREFIX` in `config.h` (C/C++ single codebase)

**Why.** `lock_unlock.h:27-31` defines `WG14_SIGNALS_ATOMIC_PREFIX` locally;
`tss_async_signal_safe.c.ipp` uses the awkward `std::`-line-split idiom
(`:60-64`, `:70-73`) because the prefix isn't available there; the public
headers don't use it at all. The sibling centralises it
(`../wg14_atomic_waits/atomic_wait.h:66-74`).

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
`atomic_uint`/`std::atomic_uint` split in `tss_async_signal_safe.c.ipp:58-76`
to use `WG14_SIGNALS_ATOMIC_PREFIX atomic_uint count;` etc. This eliminates the
C/C++ drift (analysis.md 4.9, Y8) and guarantees the `extern "C"` declarations
in the headers are genuinely compiled from C++ TUs (AGENTS.md rule 4's purpose).

### 4.2 `_WG14_SIGNALS_STATIC_ASSERT` helper

**Concrete change.** Add to `config.h` (port of
`../wg14_atomic_waits/atomic_wait.h:146-150`):

```c
#ifndef WG14_SIGNALS_STATIC_ASSERT
#ifdef __cplusplus
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif
#endif
```

Use it (with the sibling's literal, namespaced message text) wherever a
compile-time guarantee belongs — e.g. a `sizeof(sigset_t) <= UINT32_MAX` check
for the Windows `sigset_t` redefinition (`thrd_signal_handle.h:43-63`, see 4.4)
and for the `SIGFENCE_COUNT_ARGS_MAX8` `__VA_OPT__` requirement
(`thrd_signal_handle.h:85-86`). These become the targets of the compile-fail
suite (§6.7).

### 4.3 `WG14_SIGNALS_NULLPTR` C23-awareness (commit the pending delta)

`config.h:69-75` already matches the sibling's
`__STDC_VERSION__ >= 202300L || __cplusplus` — this is exactly the uncommitted
working-tree delta. Commit it.

### 4.4 Windows `sigset_t`: bound-check the bit shifts

**Why.** `thrd_signal_handle.h:52-63` compute `1u << (signo - 1)` with no range
check; `signo > 32` is UB (analysis.md 4.5), and `sigaddset`/`sigismember`
are also called from signal-handler context.

**Concrete change.** Add the bounds check to all four helpers, e.g.:

```c
static inline void sigaddset(sigset_t *ss, const int signo)
{
  if(signo >= 1 && signo <= 32)
    *ss |= (1u << (signo - 1));
}
```

and keep `sigismember` total (return false out of range) so the Windows
`sigfillset_*` lazy checks don't read a torn set.

### 4.5 Backend `.ipp` include guards + platform `#error` guards

**Why.** `thrd_signal_handle_posix.c.ipp` / `_windows.c.ipp` / the shared
`tss_async_signal_safe.c.ipp` have no include guards of their own; double
inclusion is prevented only by the parent headers' guards and the
`#ifdef _WIN32` dispatch in `thrd_signal_handle.h:463-469`. The sibling adds
per-file guards *and* a `#error` guard so a wrong-platform include is a clear
compile error instead of a silent mis-compile
(`../wg14_atomic_waits/atomic_wait_macos.c.ipp:20-25`).

**Concrete change.** Wrap each backend `.ipp` in
`#ifndef WG14_SIGNALS_<NAME>_IMPL_GUARD / #define ... / #endif`, and for the
POSIX file add:

```c
#ifdef _WIN32
#error "thrd_signal_handle_posix.c.ipp must only be included on non-Windows"
#endif
```

---

## 5. Implementation techniques (concrete defect fixes)

### 5.1 `signal_decider_create`: unlock before the warning `continue` (fixes 1.1, Y1)

**Why.** `thrd_signal_handle_common.ipp.ipp:505-513` — when a guarded signal has
no installed handler, the WARNING is printed and the loop `continue`s **without
`UNLOCK(state->lock)`**. The global spinlock is then held forever: every later
`stdc_raise`/`siginstall`/decider call spins (verified 100% CPU deadlock).

**Concrete change.** At `:513`:

```c
if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
{
  WG14_SIGNALS_STDERR_PRINTF(
    "WARNING: signal_decider_create() installing decider for signal %d but "
    "handler was never installed for that signal.\n", signo);
  *retp++ = WG14_SIGNALS_NULLPTR;   /* keep slot layout aligned — see 5.2 */
  UNLOCK(state->lock);
  continue;
}
```

**Verification.** New regression test: `siginstall` SIGUSR2 only, then
`signal_decider_create({SIGUSR1,SIGUSR2})`; the next `stdc_raise(0,...)` must
return instead of hanging. (This is the exact 1.1 reproduction.)

### 5.2 `signal_decider_create`/`destroy` slot alignment (fixes 1.2, AA1) [DONE]

**Why.** `create` advances the decider-pointer slot (`*retp++`, `:539`) only
for guarded signals with an installed handler, while `destroy` advances it for
*every* guarded signal (`:603-610`). With any mixed installed/not-installed
guarded set the layouts misalign and `destroy` frees a node still linked on the
*other* signal's `global_handler` list → use-after-free on the next raise
(verified SIGBUS, 1.2).

**Concrete change.** The minimal, layout-preserving fix is the `*retp++ = NULL`
line shown in 5.1 — create and destroy then both advance one slot per guarded
signal and NULL slots are skipped. The robust fix (recommended) is to stop
storing a positional array at all: allocate the handle as
`{ sigset_t guarded; struct global_signal_decider_t **decs[NSIG]; }` indexed by
signo, and have `destroy` do `if(guarded_member(signo) && decs[signo])`. Either
way, `destroy` must free only slots that `create` populated.

**Verification.** Regression test: install SIGUSR2, create decider for
{SIGUSR1,SIGUSR2}, destroy it, then `stdc_raise(SIGUSR2)` must not fault; then
re-install and repeat 100× under ASan. (This is the 1.2 reproduction; 5.1 must
land first.)

**Status: DONE (2026-08-09).** The `*retp++ = WG14_SIGNALS_NULLPTR` line from §5.1
was applied at `thrd_signal_handle_common.ipp.ipp:514`. The exact reproduction
above crashes with an ASan heap-use-after-free in `stdc_raise` against the pre-fix
library and passes 100 iterations under ASan/UBSan against the fixed build.

### 5.3 `sig_global_tss_state_*` on the fallback path (fixes 1.3, 2.4, 2.6) [part 1 done]

**Why.** On fallback platforms (`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL == 0`,
i.e. macOS and any non-GNU/MSVC platform), `*sig_tss_state_raw()` is a NULL
static until `siginstall()` runs (`thrd_signal_handle_common.ipp.ipp:238-243`).
`sig_global_tss_state_init` (`:264-268`) passes that NULL straight to
`tss_async_signal_safe_thread_init(NULL)` which dereferences `mem->lock` →
SIGSEGV (verified, 1.3). The documented one-line setup `stdc_raise(0, NULL,
NULL)` therefore crashes on macOS and aborts on Windows (1.4).

**Concrete change.** Three parts:

1. Make `sig_global_tss_state_init` self-creating (`:264-268`):

```c
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_init)(void)
{
  if(*WG14_SIGNALS_PREFIX(sig_tss_state_raw)() == WG14_SIGNALS_NULLPTR)
  {
    if(-1 == WG14_SIGNALS_PREFIX(sig_global_tss_state_create)())
    {
      return -1;
    }
  }
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
      *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
}
```

2. Fix the dead code in `sig_global_tss_state_destroy` (`:276-281`) — the
   `*sig_tss_state_raw() = NULL` after `return` never runs (2.4), leaving a
   freed TSS dangling for the next `stdc_raise` (Z3):

```c
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_destroy)(void)
{
  const int ret = WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
      *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)() = WG14_SIGNALS_NULLPTR;
  return ret;
}
```

3. Make `tss_async_signal_safe_create/destroy/thread_init/get` NULL-safe
   (2.6): return `-1`/`NULL` with `errno = EINVAL` when `val` is NULL.

**Verification.** New regression test (the 1.3 reproduction): on the fallback
path, `stdc_raise(0, NULL, NULL)` and a bare `sigguarded(...)` must not crash
without any prior `siginstall()`. This test must run on macOS (fallback) and
must also pass on Linux under `-DALWAYS_USE_FALLBACK_TLS=ON` (§2.1).

**Status: part 1 DONE (2026-08-09).** The self-creating `sig_global_tss_state_init` was
applied at `thrd_signal_handle_common.ipp.ipp:264-272`, fixing 1.3. Regression test
`test/standalone_setup_test.c` reproduces the crash (ASan SEGV at
`tss_async_signal_safe.c.ipp:177`, address 0x10) before the fix and passes 6/6 afterwards.
Parts 2-3 (2.4 dead code, 2.6 NULL-safe tss API) remain open.

### 5.4 `tss_async_signal_safe_thread_deinit`: count decrement under the lock (fixes 2.1, X1/X2)

**Why.** `tss_async_signal_safe.c.ipp:160-166`: the `atomic_fetch_sub(&state->count)`
and `free(state)` happen *after* `UNLOCK`. Two threads sharing the same tss
exiting simultaneously can free `state` while the other thread still decrements
it or reads `state->val` → UAF (verified sequential UAFs in X1/X2/Z3).

**Concrete change.** Hold the lock across the decrement (the sibling's rule —
never release a lock while an object's lifetime is still in play):

```c
UNLOCK(mem->lock);   /* after the map erase */
if(1 == atomic_fetch_sub_explicit(
    &state->count, 1, WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel))
{
  free(state);
}
```

becomes: perform the `fetch_sub` while still holding `mem->lock`, and only if
it returned 1, keep the lock and `free(state)` before `UNLOCK`. Additionally,
`tss_async_signal_safe_destroy` (`:111-134`) must stop freeing `mem` while
deinit callbacks may still hold `state->val == mem`; document that destroy
requires all threads joined, or defer `free(mem)` via the atexit list.

**Verification.** The existing `async_signal_safe_tls_test.c` plus a new
two-thread-same-tss concurrent-exit stress (1000 iterations) under TSan
(§3.2).

### 5.5 `tss_async_signal_safe_thread_init`: re-check under the lock (fixes 3.13, 3.14, 2.5)

**Why.** `:184-200`: the user `create` callback runs outside `mem->lock`, then
the map is inserted without re-checking. Two racing threads both run `create`
(leak of the loser's value, double `count`, double atexit registration —
3.13); a `create` returning 0 with NULL leaves `errno`/state inconsistent
(2.5); a failed `thread_atexit` leaves a committed entry and count (3.14).

**Concrete change.** Port the sibling's "capture/check under the lock" idiom
(`../wg14_atomic_waits/atomic_wait_common.ipp.ipp:556-589`):

```c
if(WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
{
  UNLOCK(mem->lock);
  void *newitem = WG14_SIGNALS_NULLPTR;
  int ret = mem->attr.create(&newitem);
  if(ret != 0 || newitem == WG14_SIGNALS_NULLPTR)
  {
    if(ret == 0) { ret = -1; errno = EINVAL; }   /* 2.5: 0+NULL is failure */
    return ret;
  }
  LOCK(mem->lock);
  it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_get)(
      &mem->thread_id_to_tls_map, mytid);
  if(!WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
  {
    /* another thread won the race; discard our copy */
    UNLOCK(mem->lock);
    mem->attr.destroy(newitem);
    return 0;
  }
  /* ... insert, bump count, and register thread_atexit LAST ... */
}
```

and if `thread_atexit` fails, roll back the insert and count before returning
-1 (3.14).

### 5.6 `tss_async_signal_safe_get`: lock-free read via a per-thread cached pointer

**Why.** `get` (`:226-243`) takes `mem->lock`, which is also taken inside
signal-handler context (`sig_global_tss_state` → `tss_async_signal_safe_get`).
A spinlock in a handler deadlocks if the interrupted thread holds it (3.1) —
so the "ASYNC-SIGNAL-SAFE" claim (§7 of analysis.md) is false in general. The
sibling's `current_thread_id()` solves exactly this by caching in TLS and only
falling back to a slow path on cache miss
(`../wg14_atomic_waits/current_thread_id.h:58-72`); `tss_async_signal_safe.c.ipp`
already does the same for the TID (`my_current_thread_id`, `:81-91`).

**Concrete change.** Cache the per-thread *value* in a plain
`WG14_SIGNALS_THREAD_LOCAL` slot set by `thread_init` (outside handler
context), and make `get` read only the cache:

```c
static WG14_SIGNALS_THREAD_LOCAL void *my_value;   /* set in thread_init */

void *WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(tss_async_signal_safe val)
{
  (void) val;
  return my_value;
}
```

`my_value` is written only by `thread_init` on the thread itself, so the read is
lock-free and race-free; the hash table remains solely for `destroy()`'s
iteration over all threads. `thread_init` still writes `my_value` before
returning, so a handler firing after `thread_init` sees the value. This makes
the async-signal-safety claim *actually true* on the fallback path (and removes
the §3.1 deadlock vector from the hot path). Keep the map write for
`destroy`/deinit bookkeeping.

### 5.7 POSIX `install_sighandler_impl`: fix `SA_NOCLDWAIT` and add `SA_RESTART` (fixes 3.3)

**Why.** `thrd_signal_handle_posix.c.ipp:371-383` installs
`SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER` for *every* signal. `SA_NOCLDWAIT` on
SIGCHLD changes process child-reaping semantics (auto-reap; `waitpid` →
ECHILD) for the whole tenure of the install — `siginstall(NULL)` silently
breaks the host app's waitpid. No `SA_RESTART` means syscalls return EINTR
during the tenure even if the pre-existing handler had SA_RESTART.

**Concrete change.** Compute flags per signal:

```c
sa.sa_sigaction = WG14_SIGNALS_PREFIX(raw_signal_handler);
sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;   /* no SA_NOCLDWAIT */
```

i.e. add `SA_RESTART` unconditionally and *never* set `SA_NOCLDWAIT` (drop it;
if the pre-existing handler had it, it's preserved in `old_handler` and
restored on uninstall anyway, and it only ever applied to SIGCHLD). Document
the `SA_NODEFER` re-entrancy trade-off in the header (already partially
documented at `thrd_signal_handle.h:408-412`).

### 5.8 `current_thread_id` portable fallback (fixes 4.1)

**Why.** `current_thread_id.c.ipp:70-72`: the final `#else` calls
`pthread_getthreadid_np()`, which exists only on FreeBSD; every other non-Linux
non-Apple POSIX platform fails to compile.

**Concrete change.** Replace `:71` with the portable
`(WG14_SIGNALS_PREFIX(thread_id_t)) pthread_self();` (keeping the FreeBSD
branch for the native call). Also `#ifdef` the FreeBSD `#include <pthread_np.h>`
(`:36-38`) — it currently compiles in unconditionally.

### 5.9 `sigfillset_*` sets: drop `__attribute__((constructor))`, force init under the lock (fixes 7.1, C11 compliance)

**Why.** `thrd_signal_handle_posix.c.ipp:49-67` (and the async sets) use
`static __attribute__((constructor))` — a GNU extension, violating AGENTS.md
rule 1 — plus a double-checked write race on `v` (7.1) for any platform where
the constructor is ignored.

**Concrete change.** Remove the attribute; build the three sets once from
`sig_global_tss_state_create()` (normal context, already under `state->lock`),
and have the `sigfillset_*` functions read only the pre-built statics (their
own lazy init path becomes a benign fallback for "called before any library
call", which in a single-threaded pre-main context cannot race). This keeps
the functions read-only afterwards, preserving the "ASYNC-SIGNAL-SAFE" claim.

### 5.10 Windows backend concrete fixes (1.4, 1.5, 1.6, V2)

- **`stdc_raise(0, ...)` aborts (1.4) [DONE]** — `thrd_signal_handle_windows.c.ipp:252-300`
  has no `signo == 0` short-circuit (POSIX has one at
  `thrd_signal_handle_posix.c.ipp:281-285`), so the documented setup call hits
  `default: abort()` in `win32_exception_code_from_signal` (`:112-134`), as
  does every signo outside {SIGABRT, SIGBUS, SIGILL, SIGSEGV, SIGFPE}. Add
  after the tss init block:

  ```c
  if(signo == 0)
  {
    return false;   /* caller is doing the non-async-safe setup */
  }
  ```
  Applied 2026-08-09 at `thrd_signal_handle_windows.c.ipp:263-267`; regression test
  `test/stdc_raise_zero_test.c` added and the Windows-excluded `stdc_raise(0, ...)` legs of
  `test/standalone_setup_test.c` re-enabled. (The broader V4 abort for non-signo-0
  unsupported codes is still open.)

- **`prepare_rsi` OOB reads (1.5) [DONE]** — `:186-192` reads
  `ExceptionInformation[1]`/`[2]` with no `NumberParameters` check; a genuine
  x64 access violation has `NumberParameters == 2`, so `[2]` (the "NTSTATUS")
  is garbage on virtually every real fault. Guard:

  ```c
  if(ptrs->ExceptionRecord->NumberParameters >= 3)
    rsi->error_code = ...ExceptionInformation[2];
  if(ptrs->ExceptionRecord->NumberParameters >= 2)
    rsi->addr = ...ExceptionInformation[1];
  ```
  Applied 2026-08-09 at `thrd_signal_handle_windows.c.ipp:188-199`; `prepare_rsi`'s
  initial `memset` keeps the unguarded fields 0. No POSIX regression (7/7 tests pass).

- **Dangling frame in Windows `stdc_raise` (1.6) [DONE]** — `:265-299` pushes a frame
  on `tss->front` but only pops it on the `setjmp`-return path; the
  `RaiseException`-returns path leaves `tss->front` pointing at a dead stack
  frame, so `win32_vectored_exception_function` (`:357-367`) later longjmps
  into dead memory. Pop the frame before returning:

  ```c
  RaiseException(...);
  tss->front = old;
  return true;
  ```
  Applied 2026-08-09 at `thrd_signal_handle_windows.c.ipp:310-314`. Still open: the
  `__except`-unwind path (a `sigguarded` catch unwinds past `stdc_raise` before it can
  pop) and V2's NULL per-thread `tss` deref.

- **NULL `tss->front` deref from a fresh thread (V2)** — the vectored handler
  calls `sig_global_tss_state()` on threads that never ran
  `sig_global_tss_state_init`; on the fallback path that returns NULL and
  `tss->front` dereferences it. Guard:

  ```c
  struct ... *tss = WG14_SIGNALS_PREFIX(sig_global_tss_state)();
  if(tss != WG14_SIGNALS_NULLPTR && tss->front != WG14_SIGNALS_NULLPTR)
  {
    longjmp(tss->front->buf, 1);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
  ```

- **`install_sighandler` count-before-create (2.3)** —
  `thrd_signal_handle_common.ipp.ipp:316-323` increments `sighandlers_count`
  before the `sig_global_tss_state_create()` check; on failure a handler stays
  installed that can never be uninstalled and the count desyncs. Reorder so the
  increment happens only after the create succeeds, rolling back the map entry
  otherwise.

### 5.11 `siguninstall`/`signal_decider_destroy` locking hygiene

- **`signal_decider_destroy` takes `state->lock` once per signal** (analysis.md
  §9): `:564-611` loops `LOCK/UNLOCK` around each signal's map access. Acquire
  once before the loop and release after.
- **`siguninstall` `-1` failure path leaks `ss`** (analysis.md §9): `:423-426`
  `return -1` before `free(ss)`; free first. (`uninstall_sighandler` always
  returns true so the path is currently dead, but keep it correct.)
- **Container UAF during concurrent `siguninstall` + `stdc_raise` (2.2)**:
  `stdc_raise` (POSIX `:314-368`) releases `state->lock` around each decider
  call; a `siguninstall` in that window can free the `sighandler_info` container
  while the raise still holds `it`. Mirror the sibling's `use_count` pattern
  (`../wg14_atomic_waits/atomic_wait_common.ipp.ipp:93-97, 641-649`): give the
  container a refcount incremented under the lock when the raise fetches the
  entry and decremented at the end; defer the container `free` until the count
  and the in-flight-refcount are both zero (reuse the existing
  `deferred_frees` list for containers too).

---

## 6. Testing techniques (concrete designs)

### 6.1 `test_wait_until`: bounded spin handshake (anti-flake)

**Concrete change.** Port `../wg14_atomic_waits/test/test_common.h:109-138`
into `test/test_common.h` verbatim (adapted to `WG14_SIGNALS_*` names): a
`test_wait_until(const char *what, const atomic_int *value, int goal)` that
spins with `timespec_get` deadline and `abort()`s after 2000 ms with a named
diagnostic. Rewrite the `while(atomic_load(...) == 2) {}` handshakes in
`thrd_signal_handle_test.c:118-120` and `thrd_sigfpe_test.c` to use it.
(AGENTS.md rule 5.)

### 6.2 TSAN-aware `<threads.h>` selection

**Concrete change.** Replace `test_common.h:18-61` with the sibling's
`test_common.h:21-95`: keep the real `<threads.h>` except when
`__GLIBC__ && TSAN`, in which case use the pthread-based `thrd_*` shim with the
nested `__has_feature`/`__SANITIZE_THREAD__` probe. Required before the TSan CI
job (§3.2) can pass — glibc's `thrd_create` bypasses TSan's `pthread_create`
interceptor and the spawned thread crashes immediately.

### 6.3 `SECTION(...)` progress markers

**Concrete change.** Add `#define SECTION(name) fprintf(stderr, "<test>: " name "\n")`
to `test_common.h` and emit one before each phase of
`thrd_signal_handle_test.c` (three `puts(...)` already exist — convert them),
`thrd_sigfpe_test.c`, and `async_signal_safe_tls_test.c`. ctest echoes stderr
only on failure, so a hang becomes localisable.

### 6.4 Regression tests for the fixed bugs (each one maps to a verified analysis finding)

Add to `test/` (all `add_code_test`, C11):

| Test file | Exercise | Catches |
|---|---|---|
| `standalone_setup_test.c` **[DONE]** | `stdc_raise(0,NULL,NULL)` and bare `sigguarded` with no prior `siginstall` | 1.3, 1.4 |
| `stdc_raise_zero_test.c` **[DONE]** | `stdc_raise(0,NULL,NULL)` returns false (no abort) | 1.4 |
| `recovery_null_loop_test.c` **[DONE]** | nested `sigguarded`, inner frame has NULL recovery + decider returns invoke_recovery, genuine SIGSEGV; must fall through to outer recovery | 1.7, C1 |
| `decider_mixed_set_test.c` **[DONE]** | `siginstall({SIGUSR2})` then create+destroy decider for {SIGUSR1,SIGUSR2}, then `stdc_raise(SIGUSR2)` ×100 | 1.1, 1.2, AA1 |
| `decider_cycle_test.c` | siginstall → decider → destroy → uninstall → siginstall → decider → raise (the AA1 orphan cycle) | AA1, Z3 |
| `tss_concurrent_exit_test.c` | two threads sharing one `tss_async_signal_safe`, both `thread_init`, both exit; 1000 iterations | 2.1, X1/X2 |
| `tss_null_handle_test.c` | `create/destroy/thread_init/get` on NULL and zeroed handles | 2.6 |
| `lock_whitebox_test.c` | `#include "detail/impl/lock_unlock.h"`, lock/unlock under TSan | 3.1 discipline |

### 6.5 White-box tests (include internals directly)

The sibling's `hash_table_whitebox_test.c` includes the backend `.ipp` with
`WG14_ATOMIC_WAITS_ENABLE_HEADER_ONLY` and drives internals single-threaded.
For `wg14_signals`:

- `lock_whitebox_test.c` (above) drives `LOCK/UNLOCK` directly — a white-box
  test that calls `signal_decider_create` with a mixed set and then verifies a
  fresh `LOCK(state->lock)` succeeds would have caught 1.1 in minutes.
- A `tss_map_whitebox_test.c` that includes `tss_async_signal_safe.c.ipp`
  and drives the map insert/erase/cleanup cycle single-threaded (the
  `thread_id_to_tls_map_t` verstable instance) — the map-under-TSan coverage
  that needs no threads.

### 6.6 Leak detection through the public API

Adapt the sibling's `wait_expected_leak_test.c` discriminator: instead of
instrumentation, observe API-visible behaviour. For `wg14_signals`: after
many `siginstall`/`siguninstall` cycles with deciders destroyed, a subsequent
`stdc_raise` of a fully-uninstalled signal must return false cleanly (no
residual `deferred_frees` node reached). Concretely: loop 20,000 ×
{create decider, destroy decider} on SIGUSR1, then assert
`siguninstall(handlers)` and one final `stdc_raise(SIGUSR1) == false` under
ASan/LSan — a leaked/kept node surfaces as an ASan report or as a raise that
"finds" a handler.

### 6.7 Compile-fail test suite (port `expect_compile_fail.cmake`)

**Concrete change.** Copy `../wg14_atomic_waits/test/expect_compile_fail.cmake`
and the `add_compile_fail_test` function from
`../wg14_atomic_waits/test/CMakeLists.txt:87-110` into `test/CMakeLists.txt`.
Targets (each a `.c` + `.cpp` pair):

- `sigfence_too_many_args`: `sigfence(a,b,c,d,e,f,g,h,i)` (9 args) — must fail
  (the `__VA_OPT__` counter returns the 9th argument, producing an undefined
  `SIGFENCE_IMPL_<expr>`); matches analysis.md X10.
- `sigfence_rvalue`: `sigfence(1)` on GNU/clang — `+m` operand must be an
  lvalue (analysis.md 2.8); note this one is intentionally
  GNU/clang-only (`#if` guard in the source).
- `windows_sigset_overflow`: `sigaddset(&ss, 33)` — after the §4.4 bounds
  check this becomes a no-op rather than a compile error, so instead make this
  a `WG14_SIGNALS_STATIC_ASSERT`-backed compile-fail for the 
  `sizeof(sigset_t) <= 4` contract (4.2) on Windows.

The `expect_compile_fail.cmake` script matches the *literal* namespaced
diagnostic text (no regexes) and echoes build output, so failures are visible
in ctest logs.

### 6.8 3-TU header-only ODR test (already present — wire it into the HeaderOnly job)

`header_only_test.{cpp,1.cpp,2.cpp}` already exist; the sibling wires the same
shape as a normal ctest with `TIMEOUT`. Ensure it is part of the §3.1 HeaderOnly
job's `ctest` run (it is part of the default test set).

### 6.9 Benchmark structure (partially done)

`wg14_signals` benchmark targets are already excluded from CI via
`ctest -E benchmark`. To match the sibling: convert the two benchmark targets in
`test/CMakeLists.txt:2,4` from `add_code_test` to `add_code_example` with
`PROPERTIES EXCLUDE_FROM_ALL TRUE` so they aren't built by default at all, and
document exact reproduce commands in Readme.md (the sibling's pattern at
`../wg14_atomic_waits/Readme.md:243-249`).

---

## 7. Documentation and process

### 7.1 `docs/proposal.md` — vendor the N3924 rev 4 wording

The sibling carries the WG14 wording it implements (`docs/proposal.md`); this
repo implements N3924 rev 4 but does not vendor it. Add
`docs/proposal.md` with the N3924 rev 4 text so `plans/analysis.md` deviations
can cite "§X of the wording" instead of prose.

### 7.2 `plans/test-review-todos.md` companion

Port the sibling's `plans/test-review-todos.md` structure: for *every* item in
`plans/analysis.md`, a verdict (testable / not testable / characterization-only
/ CI-change) with the test design in §6.4-6.7, plus an explicit "do not
naively complete these" section for deliberately-untested behaviours (e.g.
Windows SEH real-fault paths, the SIGFPE trap behaviour noted in analysis.md
6.4).

### 7.3 Readme structure

Add the sibling's CMake-options table and a "Supported targets / CI" list that
matches the actual `.github/workflows/ci.yml` (the sibling's
`Readme.md:110-192`). Also remove the stale "Known bugs" gap noted in
analysis.md §9 (`Readme.md:139` lists only the pcpp future work).

### 7.4 `.gitattributes` trim

`wg14_signals/.gitattributes` (102 lines) references `cmake/headers.cmake`,
`cmake/interface.cmake`, `cmake/sources.cmake`, `cmake/tests.cmake` which do
not exist. Trim to the sibling's 12-line shape (`../wg14_atomic_waits/.gitattributes`).

---

## 8. What NOT to adopt (cautionary notes)

- **The `cmake_minimum_required(3.15)` + `PROJECT_IS_TOP_LEVEL` mismatch is
  shared by both projects** (3.21 feature). Fix it here (bump minimum to 3.21
  or replace the guard); don't copy the sibling's latent defect.
- **The sibling's `ALWAYS_USE_PTHREADS_BACKEND` cannot build on Windows/MSVC**
  (no `<pthread.h>`); their docs still claim "every platform". For the §2.1
  `ALWAYS_USE_FALLBACK_TLS` option, there is no such problem (it is pure
  compile-definition), but document that forcing the fallback on Windows is
  fine — just be aware `tss_async_signal_safe` is then exercised with MSVC's
  `/experimental:c11atomics`.
- **The hash-table proxy engine is tuned for wait/notify**; adopt the
  *techniques* (§5.4-5.6: re-check-under-lock, refcounted containers,
  lock-free cached reads) rather than the structure wholesale. Replacing
  `verstable.h` with a triangular-probing table (an idea from the earlier
  revision of this doc) is a larger refactor; the concrete bugs it would fix
  (Z1: uninitialised verstable map on NSIG≥1024) are better fixed first by
  initialising the map instance (call `signo_to_sighandler_map_t_init(&...)`)
  in `sig_global_state()` — a one-line fix that removes the entire
  NSIG≥1024 hazard class without touching the table.
- **CI breadth**: 7 jobs/~80 legs in the sibling is proportional to its
  problem; for `wg14_signals` prioritise HeaderOnly + TSan + FreeBSD
  (§3.1-3.3) and defer the Fil-C and fallback-TLS matrix breadth until the
  §1 quick wins land.

---

## 9. Priority-ordered adoption plan

| # | Change | Location | Fixes (analysis.md) | Effort |
|---|--------|----------|--------------------|--------|
| 1 | Packaging: `configure_package_config_file` + version file + `install(DIRECTORY include/...)` | `CMakeLists.txt:10,58-70` | V1 | Small |
| 2 | `WG14_SIGNALS_EXTERN` → `static WG14_SIGNALS_INLINE` in header-only mode | `config.h:109-115` | 1.8, C3, Y8, Y10 | Small |
| 3 | `signal_decider_create`: unlock + align slot on the warning path **[DONE]** | `thrd_signal_handle_common.ipp.ipp:505-514,539` | 1.1, 1.2, AA1 | Small |
| 4 | `signal_decider_destroy` slot alignment / signo-indexed handle **[DONE]** | `:564-611` | 1.2 | Small |
| 5 | Fallback-path setup: self-creating tss init + dead-code fix + NULL-safe tss API | `:264-281`, `tss_async_signal_safe.c.ipp:93-243` | 1.3, 2.4, 2.6, Z3 | Small |
| 6 | Windows `stdc_raise(0,...)` short-circuit + `prepare_rsi` bounds + frame pop + NULL-tss guard **[1.4, 1.5, 1.6 done; V2 open]** | `thrd_signal_handle_windows.c.ipp:186-192,265-299,357-367` | 1.4, 1.5, 1.6, V2 | Small |
| 7 | Install-consumer ctest | `test/install_consumer/` | V1 regression-proofing | Medium |
| 8 | TSan CI job + `tsan-toolchain.cmake` + TSAN-aware `test_common.h` | `.github/workflows/ci.yml`, `test/test_common.h` | 5.4, 2.1, 2.2, 3.1 | Medium |
| 9 | Per-test `TIMEOUT 60` | `CMakeLists.txt:88-91` | test hygiene | Trivial |
| 10 | AGENTS.md rules 4 & 5 | `AGENTS.md` | Y7, flaky tests | Trivial |
| 11 | `tss_async_signal_safe`: lock-free `get` via cached TLS value; deinit count under lock; init re-check | `tss_async_signal_safe.c.ipp:136-243` | 3.1, 2.1, 3.13, 3.14, 2.5 | Medium |
| 12 | `install_sighandler` flags: drop `SA_NOCLDWAIT`, add `SA_RESTART` | `thrd_signal_handle_posix.c.ipp:371-383` | 3.3 | Small |
| 13 | Regression tests for §9 rows 3-6 (standalone setup, mixed decider set, cycle, tss NULL) | `test/*` | 1.1-1.6, 2.1, 2.6, AA1, Z3 | Medium |
| 14 | Compile-fail suite (`expect_compile_fail.cmake` + sigfence targets) | `test/` | 5.3, 2.8, X10 | Medium |
| 15 | HeaderOnly CI job | `.github/workflows/ci.yml` | 5.4, 1.8 | Small |
| 16 | FreeBSD VM job + `stdthreads` link + portable `pthread_self()` fallback | `ci.yml`, `CMakeLists.txt:82`, `current_thread_id.c.ipp:70-72` | 5.4, 4.1 | Medium |
| 17 | Fil-C toolchain fix (`FILC_ROOT`-driven) + gate `SIGFENCE_IMPL_*` on `DISABLE_INLINE_ASM` | `cmake/filc-toolchain.cmake`, `thrd_signal_handle.h:97-131` | AA2 | Medium |
| 18 | `sigfillset_*` constructor-attribute removal + init under lock | `thrd_signal_handle_posix.c.ipp:49-127` | 7.1, C11 rule 1 | Small |
| 19 | Container refcount for `siguninstall` vs in-flight `stdc_raise` | `:314-368`, `:348-361` | 2.2 | Medium |
| 20 | Feature-test macros + MSVC c11-atomics helper | `CMakeLists.txt` | 4.6, X9 | Small |
| 21 | `docs/proposal.md` + `plans/test-review-todos.md` + Readme tables + `.gitattributes` trim | `docs/`, `plans/`, `Readme.md`, `.gitattributes` | process | Small |
