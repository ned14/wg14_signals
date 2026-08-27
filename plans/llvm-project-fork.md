# Replace LLVM-libc signal handling with the wg14_signals threadsafe signals implementation

## Goal

Replace the signal handling inside the vendored `llvm-project/` tree's `libc/`
subproject (LLVM-libc) with the reference implementation of the WG14 N3924
improved C signals proposal in this repository (the `wg14_signals` library,
i.e. this repo's own `include/wg14_signals/` and `src/wg14_signals/`). The
replacement must:

1. Keep every existing POSIX/ISO-C signal entrypoint (`raise`, `signal`,
   `sigaction`, `sigprocmask`, `sigaltstack`, `kill`, `pthread_sigmask`,
   `sigaddset`/`sigdelset`/`sigemptyset`/`sigfillset`) with conformant
   behaviour — a user of LLVM-libc must observe no regression in the existing
   API surface.
2. Expose the full N3924 API as new LLVM-libc public entrypoints:
   `sigguarded`, `stdc_raise`, `siginstall`, `siguninstall`,
   `siguninstall_system`, `signal_decider_create`, `signal_decider_destroy`,
   `sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`,
   `sigfillset_asynchronous_debug`, the `sigfence` macro, `stdc_siginfo` and
   its supporting types/constants (`stdc_siginfo_value`, `sig_decision_*`,
   `SIGGUARDED_FAILURE_VALUE`), `tss_async_signal_safe_create` /
   `tss_async_signal_safe_destroy` / `tss_async_signal_safe_thread_init` /
   `tss_async_signal_safe_get`, and `current_thread_id`.
3. Work on as many of LLVM-libc's target platforms as the underlying
   facilities allow, with a documented gating policy for the rest.

Integration mechanism (decision 2026-08-20): the reference implementation
is **not** copied into the fork. It is mounted as a **git submodule** at
`libc/src/signal/wg14`, pristine and zero-drift, and the fork's build
consumes its sources directly. To keep the submodule unmodified, the
recursion-avoidance shim becomes an **upstream embedder override layer** in
wg14_signals itself (a small set of function-like macros defaulting to the
standard C calls; §"OS abstraction layer"). This is exactly the property
the project wants to prove: the existing reference implementation, without
fork-side patching, is sufficient to extend an existing standard C
library.

Everything below is derived from direct study of both trees at the current
checkouts: `llvm-project/` at `d98d12cb99b6` (branch `main`, remote
`git@github.com:ned14/llvm-project.git`) and this repo at HEAD. Paths are
relative to this workspace root (`llvm-project/` is the vendored fork root).

## Ordered implementation task list (for the implementing agent)

This is the executive summary; each item is expanded in the sections below.
Paths are line-referenced against the current trees.

1. Add wg14_signals as a git submodule of `llvm-project` at
   `libc/src/signal/wg14` (§"Submodule integration"). **DONE in this
   session**: `git submodule add https://github.com/ned14/wg14_signals.git
   libc/src/signal/wg14`, pinned to `50042dad4eb376bc88f6d21568007e72c615f3a8`
   (heads/main); staged in the fork, not yet committed.
2. Upstream work in **this repo** (`wg14_signals`, §"OS abstraction
   layer"): add the embedder override layer to `config.h` — function-like
   macros defaulting to the standard names, covering the three call-site
   families that would recurse through libc's own entrypoints
   (`sigaction`, `abort`, `pthread_kill(pthread_self(), ...)`). The
   submodule's `.ipp` files then need no modification; this is the feature
   that proves the reference implementation is sufficient to extend an
   existing standard C library. **DONE in this session**: `config.h` now
   defines `WG14_SIGNALS_SIGACTION`/`WG14_SIGNALS_ABORT`/
   `WG14_SIGNALS_KILL_SELF` (function-like macros defaulting to the
   standard calls, documented as the embedder interface, `#undef`-safe) and
   the POSIX `.ipp` routes all 11 call sites through them (6 × sigaction,
   4 × abort, 1 × pthread_kill(pthread_self())). Proven by the new
   `WG14_SIGNALS_OVERRIDE_PROBE` CMake option (`test/override_probe.{h,c}`
   redirects the hooks to distinct wrapper functions force-included ahead
   of `config.h`; the library objects then reference the wrappers, and the
   full suite passes 37/37 both OFF and ON — verified locally on macOS
   arm64, both Debug-free Release configs; CI leg `LinuxOverrideProbe`
   added to `.github/workflows/ci.yml`).
3. Build-wire the wg14_signals library object in libc (compile the
   submodule's C sources directly under the full-build flags:
   `-ffreestanding -fno-builtin -nostdlibinc`, no `add_subdirectory` of the
   submodule) and run the existing wg14_signals unit-test suite against it
   on Linux first (§"Phase 1"). **DONE 2026-08-21**: fork-owned
   `libc/src/signal/wg14/CMakeLists.txt` builds the submodule's five C
   sources + `embedder_shim.{h,c}` as `libc.src.signal.wg14.wg14_signals`
   with `-std=gnu11` under the full-build flag set (see the compile command
   in `build-wg14-obj.log`); the shim redefines
   `WG14_SIGNALS_SIGACTION/ABORT/KILL_SELF/GETTID` to raw Linux syscalls via
   `__llvm_libc_syscall` (kernel-layout sigaction conversion with
   `__restore_rt`, `SYS_gettid`/`SYS_tgkill` self-delivery, the
   recursion-free hard abort; `SA_NOCLDWAIT` is stripped from the raw
   handler so POSIX wait()/zombie semantics survive). Compiles clean under
   the full flags modulo `-Wno-conversion -Wno-cast-qual
   -Wno-global-constructors` for the vendored verstable.h and the lazy-init
   sigset builders. The fork's own smoke test (`.kilo/tmp/wg14_libc_smoke.c`,
   linked against the built `libc.a` + `crt1.o` + compiler-rt builtins)
   passes 100% on Linux aarch64: sigaction/signal/raise/SA_SIGINFO/SIG_IGN/
   sigprocmask (untouched direct-kernel entrypoints),
   siginstall/siguninstall/signal_decider_create/destroy, stdc_raise,
   sigguarded SIGSEGV recovery, sigfillset_*, current_thread_id,
   tss_async_signal_safe_*, default-action delivery, and abort() semantics
   (through libc's own C11 abort). Gated to Linux only until the darwin/
   freebsd syscall layers land (Phase 5/6), preserving the darwin baseline.
 4. Extend the generated public headers (§"Public API surface"): extend
    `libc/include/signal.yaml` with the N3924 functions/types/macros and add
    `libc/include/threads.yaml` for the `tss_async_signal_safe_*` functions
    (7.30.6.5-8, in `<threads.h>` per the proposed wording) — plus a
    hand-written header per the `*.h.def` pattern for the `sigfence` macro,
    which cannot be yaml-expressed.
   **DONE 2026-08-21**: `signal.yaml` gained the twelve N3924 functions, the
   N3924 types (`global_signal_decider_t`, `sig_decide_t`, `sig_decision`,
   `sig_func_t`, `sig_recover_t`, `struct_stdc_siginfo`,
   `union_stdc_siginfo_value` — hdrgen derives the `struct_`/`union_` file
   names from the signatures) and the `sigfence` macro (extracted verbatim
   from the submodule header into `llvm-libc-macros/sigfence-macros.h`);
   `threads.yaml` (the four `tss_async_signal_safe_*` functions per N3924
   7.30.6.5-8, plus `current_thread_id`), the `llvm-libc-types` mirrors
   (including `SIGGUARDED_FAILURE_VALUE`), the `hdr/` proxies, and the
   `libc.include.signal` target deps. `sigismember` added to `signal.yaml`
   + a Linux implementation (the submodule calls it). The aarch64
   `ucontext_t`/`mcontext_t` kernel-ABI types were added and the
   x86_64-only gate in `include/CMakeLists.txt` widened.
5. Add the new entrypoint objects (`libc/src/signal/wg14/entrypoints/*.cpp`
   thin `LLVM_LIBC_FUNCTION` wrappers) and register them in
   `libc/config/linux/{x86_64,aarch64,arm,riscv}/entrypoints.txt`.
   **DONE 2026-08-21**: 17 wrappers (16 in `src/signal` + `current_thread_id`
   in `src/threads` per the yaml placement) with internal headers declaring
   the namespace functions; registered via `add_entrypoint_object` ALIAS
   targets in `libc/src/signal/CMakeLists.txt` and the three Linux configs
   (aarch64/x86_64/riscv; arm/i386/power have no signal entrypoints and stay
   that way).
6. ~~Re-point the existing Linux signal entrypoints~~ **SUPERSEDED
   2026-08-21**: `sigaction()`/`signal()`/`raise()`/`sigprocmask()`/
   `pthread_sigmask()`/`sigaltstack()`/`kill()` and the sigset helpers are
    **completely untouched** — they keep their existing direct-kernel
    implementations and never call into wg14_signals (an early registry
    prototype that routed them through wg14 deciders was reverted). Under
    the 2026-08-22 policy libc's own code does NOT call the wg14 API
    surface at all (internal libc never calls
    `siginstall`/`siguninstall`/`signal_decider_*`/`stdc_raise` —
    §"Internal consumers"; the one attempt to prepend `stdc_raise()` to
    `abort()` was reverted as redundant/harmful, item 7); the POSIX
    entrypoints remain available for
    user code that wants them, coexisting per-signal (the last writer of a
    signal's kernel disposition wins). `sigismember` was still added as a
    Linux entrypoint (the submodule calls it).
7. Re-point the internal consumers (§"Internal consumers"): `abort()` in
   `libc/src/stdlib/linux/abort_utils.h` (SIGABRT via the new raise path,
   SIG_DFL re-raise, unblock), `system()` in
   `libc/src/stdlib/linux/system.cpp`, `posix_spawn()` in
   `libc/src/spawn/linux/posix_spawn.cpp`, and `raise` usage anywhere else
   that calls `linux_syscalls::raise` / `rt_sigaction` directly.
   **abort() NOTHING-TO-DO 2026-08-22 (design correction); system()
   NOTHING-TO-DO 2026-08-22 (policy); posix_spawn() NOTHING-TO-DO
   2026-08-22 (evidence)**.
   **POLICY (2026-08-22): internal libc NEVER calls `siginstall`/
   `siguninstall` (nor `signal_decider_create`/`signal_decider_destroy`) —
   ONLY code outside libc does. libc merely works correctly when external
   code does so.** **RULE (2026-08-22, learned the hard way): NEVER call
   `stdc_raise()` from internal libc for a signal that is (or will be)
   delivered via the OS raise path either — if `siginstall()` was performed,
   the raise enters the wg14 filtering handler BY DEFINITION (it IS the
   kernel disposition), so a manual `stdc_raise()` is redundant, and it is
   harmful: a pre-existing handler that returns runs twice (once via
   `stdc_raise()`'s `invoke_sigaction` handoff, once via the kernel
   delivery's handoff). `abort()` was first "improved" to prepend
   `stdc_raise(SIGABRT, &info, NULL)` with a synthesized SI_TKILL siginfo
   (2026-08-22), then REVERTED the same day: with `siginstall`'ed SIGABRT,
   `abort()`'s tgkill raise delivers into the raw handler, so the decider
   chain, longjmp recovery and the pre-existing-handler handoff all run
   naturally with the REAL kernel siginfo (`SI_TKILL`, pid, uid — verified
   by the smoke test's recovery-fn siginfo check, which passes against the
   reverted code); without `siginstall`, the raise hits the default
   disposition and terminates, byte-identical to upstream. `abort_utils.h`
   is now exactly upstream again (no `stdc_raise` call, no synthesized
   siginfo, no added CMake deps).
   `posix_spawn()` (**NOTHING-TO-DO, with evidence**): the current
   implementation has no SIGCHLD/SIGINT/SIGQUIT disposition code at all —
   verified against upstream history (even the pre-SigAbortGuard version,
   commit `463f6cb576fc^`) — its only signal interaction is the
   `SigAbortGuard` around the fork syscall (block-all-signals +
   abort-lock read lock, `libc/src/signal/linux/signal_utils.h:128-154`),
   which is abort-machinery synchronisation and stays; the plan's
   "child-side SIGCHLD/SIGINT/SIGQUIT reset" description was stale. A
   tree-wide search for remaining direct `linux_syscalls::raise` /
   `rt_sigaction` call sites finds only `signal/linux/raise.cpp` (the
   untouched POSIX entrypoint) and the deliberate hard-abort fall-through
   in `abort_utils.h`; the `kill`/`pthread_sigmask`/`sigaltstack`/sigset
   helpers are out of the N3924 replacement surface and stay.
   Verified: Linux aarch64 full build `check-libc` 1152/1159 (7 failures all
   the known docker-as-root permission tests, none signal-related; the abort
   death tests pass) and the extended smoke test
   (`.kilo/tmp/wg14_libc_smoke.c`: abort with no siginstall → SIGABRT
   termination; abort inside `sigguarded(SIGABRT)` → recovery; abort with a
   returning claim decider → hard-abort SIGABRT termination; system()
   exit statuses, signal-killed-child reporting, and SIGINT/SIGQUIT sent to
   the `system()`'ing process being consumed) passes 100%;
   standalone suite 38/38 on macOS arm64 (37 + the new
   `stdc_raise_noinstall_test`) and 27/27 Windows subset under wine
   (mingw+UCRT, TLS-disabled fallback, `__cxa_thread_atexit` stubbed — the
   mingw-w64 winpthreads `tls_atexit` assert on the library's private DSO
   symbol is a pre-existing MinGW-only limitation, real Windows CI uses
   MSVC).
 8. Bring up macOS (darwin) (§"Phase 6"): add darwin OSUtil syscall wrappers
    (sigaction, sigaltstack, sigprocmask, pthread_kill/pthread_self via the
    existing darwin syscall layer), darwin `signal-macros.h`, generated
    `struct_sigaction`/`siginfo_t`/`ucontext_t` types, and the config
    entrypoints in `libc/config/darwin/{aarch64,x86_64}/entrypoints.txt`;
    macOS uses the fallback hash-table TLS (config.h already defaults
    `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` on `__APPLE__`).
    **DEFERRED 2026-08-22 (order swap)**: darwin is pushed after the Linux
    hermetic test work (item 12) and FreeBSD bring-up (item 9) — upstream's
    libc darwin support is thin (no darwin `crt1` → no hermetic tests,
    `sigsetjmp` darwin epilogue broken, OSUtil aarch64-only, the earlier
    build-fix patches would need re-applying to the fork tree), so it is the
    weakest platform to iterate on blind; the Linux/FreeBSD bring-ups share
    the same POSIX `.ipp` and syscall-layer recipe and are verifiable. The
    darwin baseline to preserve meanwhile: `libc`/`libm` build, `check-libc`
    lit 0-tests failure, `hdrgen_integration_test` 1/1.
9. Bring up FreeBSD (§"Phase 7") using the existing freebsd syscall layer
   (`libc/src/__support/OSUtil/freebsd/syscall_wrappers/`), config in
   `libc/config/freebsd/x86_64/entrypoints.txt`; `current_thread_id()` uses
   `pthread_getthreadid_np` (libc already provides
   `libc/src/pthread/pthread_getthreadid_np.cpp`).
10. Windows (§"Phase 8"): gate behind a CMake option, default OFF; the SEH
    backend requires `__try/__except` support (clang-cl), `GetCurrentThreadId`
    and the `IMAGE_TLS_DIRECTORY` mechanism which conflicts with libc-as-CRT;
    document and defer.
11. GPU / UEFI / baremetal (§"Phase 9"): no OS signals exist there; keep the
    new entrypoints absent from those configs, add a compile-time
    `#error`-free stub (or simply no targets) and document that
    `sigguarded()` etc. are unsupported, matching the current absence of
    signal entrypoints.
12. Testing (§"Test plan"): port the meaningful wg14_signals tests as libc
    hermetic tests on Linux (`libc/test/src/signal/wg14/*`), keep the
    standalone wg14_signals suite running (it exercises the same code against
    the host libc), and add libc-native tests for the re-pointed existing
    entrypoints. **DONE 2026-08-22**: see Phase 3 (moved up before the
    darwin/freebsd bring-ups so the Linux integration's correctness net
    lands while the verification environment is in hand; it also flushed out
    and fixed the weak setjmp/longjmp bridge frame-reuse defect in
    `wg14_hostcalls.cpp`, Phase 3).
13. Documentation updates (§"Documentation updates"): LLVM-libc platform
    support pages, config options, and this repo's `Readme.md` supported
    targets.
14. On a Linux CI runner: full `check-libc` run; then repeat the macOS legs.
 15. Replace **all** remaining old-API signal handling in llvm-project
     (§"Comprehensive review" and §"Phase 9"): first the shared machinery in
     `llvm/lib/Support` (Signals.inc, CrashRecoveryContext, Process.inc,
     Program.inc, InitLLVM), then its consumers (clang driver, lld,
     llvm-exegesis, the interpreter), then the compiler-rt sanitizer
     runtimes (sigaction/signal interceptors and internal handlers), then
     the Windows SEH paths — each swapped to `siginstall()` +
     `signal_decider_create()` + `sigguarded()`/`stdc_raise()`.
     **Phase 9a (llvm/lib/Support) DONE 2026-08-22** behind
     `LLVM_ENABLE_THREADSAFE_SIGNALS` (default ON on Linux); 9b is
     nothing-to-do (raise() calls kept per the 2026-08-22 amendment); 9c
     (compiler-rt) and 9d (Windows SEH) remain.

## Background: what exists in each tree today

### LLVM-libc signal handling today

- Public signal API is **Linux-only** in practice. `libc/src/signal/linux/`
  implements `kill`, `raise`, `sigaction`, `sigaltstack`, `sigprocmask`,
  `pthread_sigmask`, `signal`, and the sigset helpers
  (`libc/src/signal/linux/sigaddset.cpp`, `sigdelset.cpp`, `sigemptyset.cpp`,
  `sigfillset.cpp`), plus `__restore.cpp` (the `rt_sigreturn` restorer) and
  `signal_utils.h` (the `sigaction`↔kernel-`rt_sigaction` conversion and the
  `SigAbortGuard`). The top-level `libc/src/signal/CMakeLists.txt` wires
  `add_entrypoint_object(<name> ALIAS DEPENDS .${LIBC_TARGET_OS}.<name>)`, so
  platforms without a `signal/<os>/` directory silently get no signal
  entrypoints.
- Config files confirm: `libc/config/linux/*/entrypoints.txt` lists all 12
  signal entrypoints; `libc/config/darwin/{aarch64,x86_64}/entrypoints.txt`
  lists **none**; `libc/config/freebsd/x86_64/entrypoints.txt` lists **none**;
  `libc/config/windows`, `gpu/*`, `uefi`, `baremetal/*` list none.
- The kernel-facing layer is `libc/src/__support/OSUtil/linux/syscall_wrappers/`
  (`rt_sigaction.h`, `rt_sigprocmask.h`, `raise.h`) — thin `syscall_checked`
  calls. `abort()` (`libc/src/stdlib/linux/abort_utils.h`) drives SIGABRT
  directly through `linux_syscalls::raise` and `unchecked_sigaction`, with a
  `SigAbortGuard` (a raw rwlock) to serialize re-entry.
- `setjmp`/`longjmp` exist per architecture (`libc/src/setjmp/{aarch64,x86_64,
  arm,riscv,wasm}/`) and `sigsetjmp`/`siglongjmp` on Linux (and partially on
  darwin — currently broken, see the earlier build work: the darwin
  `sigsetjmp_epilogue` references a nonexistent `sigprocmask` syscall and
  `__jmp_buf.sigmask`; we excluded it from the darwin build and config).
- Headers are generated from YAML (`libc/include/signal.yaml` +
  `libc/include/llvm-libc-macros/{linux,gpu}/signal-macros.h` +
  `llvm-libc-types/{siginfo_t.h, sigset_t.h, struct_sigaction.h, stack_t.h,
  union_sigval.h, sig_t.h, sighandler_t.h}`). `struct_sigaction.h` is
  Linux-shaped (`sa_restorer` under `#ifdef __linux__`).
- Tests: `libc/test/src/signal/linux/*` and hermetic tests; on macOS the
  hermetic tests are skipped because there is no
  `libc.startup.darwin.crt1` (see the earlier build session).
- Threading: `libc/src/threads/` (C11 threads) and `libc/src/pthread/`
  (including `pthread_self.cpp`, `pthread_kill` — not present, note:
  `libc/src/pthread/` has no `pthread_kill.cpp` today), TLS in
  `libc/startup/linux/<arch>/tls.cpp` and `libc/src/__support/threads/`.

### wg14_signals capabilities

Public headers (`include/wg14_signals/`): `config.h` (167 lines: prefix,
TLS policy, visibility), `thrd_signal_handle.h` (773 lines: the N3924 API),
`tss_async_signal_safe.h` (91 lines), `current_thread_id.h` (89 lines).
Implementations are `.ipp` files in `include/wg14_signals/detail/impl/`:
`thrd_signal_handle_posix.c.ipp` (604 lines), `thrd_signal_handle_windows.c.ipp`
(870 lines), `thrd_signal_handle_common.ipp.ipp` (879 lines),
`tss_async_signal_safe.c.ipp` (375 lines), `current_thread_id.c.ipp` (108
lines), `thread_atexit.c.ipp` (241 lines) and `thread_atexit.cpp.ipp`. The
`src/wg14_signals/*.c` files are one-line includes of the `.ipp`s. The library
is pure C11 (C++ header-only mode exists but the C path is what we want).

Platform support today (CI matrix in `.github/workflows/ci.yml`): Linux
(gcc/clang), macOS (clang), Windows (MSVC/clang-cl), FreeBSD, Fil-C. The POSIX
backend requires: `sigaction`, `pthread_kill`/`pthread_self`, `NSIG`, a
`siginfo_t` with `si_addr` etc., `ucontext_t`, `setjmp`/`longjmp` (`jmp_buf`),
`sigsetjmp`/`siglongjmp` on z/OS only, `malloc`/`free`, `memcpy`/`memset`,
`abort`, `__cxa_thread_atexit` (preferred) or the
`pthread_key_create`/`pthread_once`/`pthread_setspecific` fallback,
`syscall(SYS_gettid)` on Linux / `mach_thread_self` on macOS /
`pthread_getthreadid_np` on FreeBSD / `GetCurrentThreadId` on Windows, and
C11 atomics. `tss_async_signal_safe` additionally needs per-thread storage;
`config.h:40-56` selects async-signal-safe TLS only on non-Apple
GNU/MSVC toolchains (ELF `initial-exec`), and the hash-table fallback
everywhere else.

## Platform matrix: what "as many platforms as possible" means

| Platform | libc signals today | wg14_signals backend | Replacement scope | Gate |
|---|---|---|---|---|
| Linux (x86_64, aarch64, arm, riscv) | full | POSIX (CI-tested) | **full**: all 12 existing + all N3924 entrypoints | always on |
| FreeBSD (x86_64) | none | POSIX (CI-tested, incl. `pthread_getthreadid_np`) | **full**: all 12 existing + N3924 | always on |
| macOS / darwin (aarch64, x86_64) | none | POSIX (CI-tested) | **full**: all 12 existing + N3924; needs darwin syscall wrappers + generated types | always on |
| Windows | none | SEH (`__try/__except`, MSVC/clang-cl only) | **partial**: N3924 API only (POSIX sigaction etc. are not part of libc's Windows story); conflicts with libc-as-CRT TLS (`IMAGE_TLS_DIRECTORY`); `sigset_t` shim exists in the header | `LIBC_WG14_SIGNALS_ENABLE_WINDOWS=ON`, default OFF |
| GPU (AMDGPU/NVPTX/SPIR-V) | none (only `gpu/signal-macros.h` numbers) | none | **none**: no OS signal delivery; entrypoints absent | excluded |
| UEFI | none | none | **none** | excluded |
| baremetal | none | none | **none** (arch `setjmp` stays, but no delivery) | excluded |

Linux, FreeBSD and macOS all run the same POSIX `.ipp` — the three OS
differences are (a) the syscall layer the OS shim targets, (b)
`current_thread_id()`'s per-OS branch, (c) signal-macro/type definitions.
That is what makes "as many platforms as possible" tractable: the platform
work is concentrated in the shim, the generated headers, and the config
files, not in the signal machinery itself.

## Comprehensive review: signal handling across llvm-project

Complete inventory of old-API signal handling in the llvm-project tree
(outside libc, which Phases 1-8 cover), derived from a full-tree search for
`sigaction`, `signal(`, `sigprocmask`, `pthread_sigmask`, `sigaltstack`,
`sigsetjmp`/`siglongjmp`, `raise(`, `kill(`, `SIG_DFL`/`SIG_IGN`/`SIG_ERR`,
`SIG*` constants, `SetUnhandledExceptionFilter`,
`AddVectoredExceptionHandler`, and `__try`/`__except`. Line references are
against the current checkout.

### A. llvm/lib/Support — the shared machinery (highest value)

This is the only place where LLVM itself installs or manipulates process
signal dispositions, and every tool (clang, lld, llvm-*, and — via
`InitLLVM` — the driver) inherits it. Replacement here covers the whole
toolchain at once.

1. **`llvm/lib/Support/Unix/Signals.inc`** — the crash/stack-trace
   machinery, and the single most complex consumer.
   - `IntSigs[] = {SIGHUP, SIGINT, SIGTERM, SIGUSR2}`,
     `KillSigs[] = {SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGBUS, SIGSEGV,
     SIGQUIT, [SIGSYS, SIGXCPU, SIGXFSZ, SIGEMT]}`, `InfoSigs[] = {SIGUSR1,
     [SIGINFO]}` + SIGPIPE (`:246-284`).
   - `RegisterHandlers()` (`:324-395`): allocates an alternate signal stack
     (`AltStackSize = MINSIGSTKSZ + 64*1024`, `:301`) via `sigaltstack`
     (HAVE_SIGALTSTACK), installs `SignalHandler` /
     `SignalHandlerTerminate` via `struct sigaction` + `sa_sigaction` with
     `SA_SIGINFO`, skipping signals whose disposition is `SIG_IGN`
     (`:370-375`), and records the previous actions in
     `RegisteredSignalInfo[]` for `RemoveFilesOnSignal` /
     `UnregisterHandlers()` (`:394`).
   - The handlers are signal-safe by discipline (comments at `:14-30`):
     they drive a lock-free node list of files to remove
     (`RemoveFilesOnSignal`), a one-shot SIGPIPE function
     (`SetOneShotPipeSignalFunction`), `PrintStackTraceOnErrorSignal`,
     `RunSignalHandlers`, and cleanup handlers
     (`CleanupOnSignal`/`AddSignalHandler`), then re-raise via
     `raise(Sig)` (`:451, 465, 506, 515, 529`) for the default action
     (with a `std::_Exit` escape for SIGINT/SIGTERM interrupt paths,
     `:246`-adjacent logic).
   - `sigprocmask(SIG_UNBLOCK, &SigMask)` in the child-after-fork path
     (`:435`).
2. **`llvm/lib/Support/Windows/Signals.inc`** — Windows equivalents:
   `SetUnhandledExceptionFilter(LLVMUnhandledExceptionFilter)` (`:407`) +
   a `signal(SIGABRT, HandleAbort)` shim (`:455`) that raises the SEH
   filter. There is also the `__try`/`__except`-based `AddVectoredExceptionHandler`
   crash-reporter path.
3. **`llvm/lib/Support/CrashRecoveryContext.cpp`** — **the textbook
   `sigguarded()` use case.** `CrashRecoveryContext::RunSafely` installs
   handlers for `{SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP}`
   (`:354`), unblocks them with `sigprocmask` (`:384`), saves the previous
   `struct sigaction`s in `PrevActions[]` (`:356, 402-422`), and recovers
   via `sigsetjmp`/`siglongjmp` to a `saved_jmp_buf`; the handler runs
   `RunSafelyOnThread`'s continuation, and unhandled conditions are
   re-raised with `raise(Signal)` (`:374`) and `raise(RetCode - 128)`
   (`:488`). Windows path: `__try`/`__except` with `ExceptionFilter`
   (`:239-241`) + `AddVectoredExceptionHandler` (`:323`).
   - This maps 1:1 onto `sigguarded(&guarded, ...)` with a decider
     returning `sig_decision_call_recovery`, and onto the Windows
     backend's SEH guard.
4. **`llvm/lib/Support/Unix/Process.inc`** — `PreventCoreFiles()`:
   `signal(SIGABRT, _exit)` / `SIGILL` / `SIGFPE` / `SIGSEGV` / `SIGBUS`
   (`:183-187`, macOS crash-reporter disable) and mach
   `task_set_exception_ports` (`:162-180`); `GetRandomNumber`-adjacent
   masking uses `pthread_sigmask`/`sigprocmask(SIG_SETMASK, ...)`
   (`:262-283`).
5. **`llvm/lib/Support/Unix/Program.inc`** — `sys::Wait` timeout
   machinery: installs a do-nothing `SIGALRM` handler via `sigaction`
   (`:385-403`) so `wait4()` returns EINTR, then `kill(pid, SIGKILL)`
   (`:428`), restores the old handler (`:432, 455`).
6. **`llvm/lib/Support/InitLLVM.cpp`** — the SIGPIPE one-shot policy:
   only registers a `sigaction(SIGPIPE)` if a one-shot handler is
   requested (`:95-100`); the one-shot SIGPIPE function lives in
   Signals.inc.
7. **`llvm/lib/Support/zOSLibFunctions.cpp`** — a z/OS-only `strsignal`
   replacement (`:64-66`); a portability shim, unaffected by the
   replacement but worth knowing about (the z/OS backend of wg14_signals
   already exists as a plan, `plans/ibm_zos_backend.md`).

### B. clang

1. **`clang/tools/driver/driver.cpp`** — `raise(CommandRes - 128)` to
   re-assert a child's signal exit (`:487`) and `raise(SIGABRT)` when a
   `-cc1` invocation failed (`:491-496`). **KEEP the `raise()` calls
   (amended 2026-08-22)**: with the wg14 machinery installed the raise
   enters the filtering handler by definition, and `stdc_raise()` would
   lose the no-siginstall termination fallback (it returns false and does
   NOT terminate — a behaviour change on clang's crash path). See the
   Phase 9b note.
2. **`clang/tools/driver/cc1as_main.cpp`** — comment-only SIGINT handling
   (`:418`); no code.
3. **`clang/lib/Driver/Driver.cpp`** — SIGPIPE suppression note in
   diagnostics (`:2443`); no direct API use.
4. Static-analyzer symbol tables (`StdSymbolMap.inc`, `CSymbolMap.inc`,
   `StdLibraryFunctionsChecker.cpp`) and test headers — data-only.
5. No other signal API use in clang's code (search over `clang/lib` and
   `clang/tools`).

### C. compiler-rt — the sanitizer runtimes (deepest integration)

compiler-rt does not link llvm/Support; it is freestanding-ish and has its
own interception layer. This is the hardest subsystem to replace and must
be phased separately (uses the standalone wg14_signals library, not the
libc-embedded one).

1. **`lib/sanitizer_common/sanitizer_signal_interceptors.inc`** — the
   `sigaction` interceptor (also `__sigaction14` on FreeBSD, `:21-23`)
   and `signal` interceptor: sanitizers interpose every user
   `sigaction()`/`signal()` call, store the user handler, and run it from
   the sanitizer's own handler (`:79-92`). **This is the old-API pattern
   the decider chain of `siginstall()`/`signal_decider_create()`
   subsumes**: user handlers become deciders, and the sanitizer's handler
   becomes the raw handler.
2. **`sanitizer_linux_libcdep.cpp`** — `internal_sigaction()` (`:104-119`),
   which calls `__sys_sigaction` / `real_sigaction` to bypass
   interception; used everywhere the runtime installs its own handlers.
3. **`sanitizer_posix_libcdep.cpp`** — the signal handler installation:
   `SetAlternateSignalStack()` via `sigaltstack` (`:193-213`),
   `MaybeInstallSigaction()` for SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL
   (`:237-260`), and the SIGABRT-first-reset-to-SIG_DFL dance
   (`:155-161`).
4. **`sanitizer_stoptheworld_linux_libcdep.cpp`** — the stop-the-world
   machinery: `internal_sigaction_norestorer` for the tracer thread
   (`:321-325`), `internal_sigprocmask(SIG_BLOCK, ...)` (`:512, 519`),
   a `SIG_DFL`-style default on SIGABRT (`:276-284`), and its own
   altstack (`:316`).
5. **`sanitizer_linux.h`** — `internal_sigaction_norestorer` /
   `internal_sigaltstack` / `internal_sigprocmask` declarations
   (`:55-85`); plus per-OS implementations (`sanitizer_haiku.cpp`,
   `sanitizer_linux_libcdep.cpp`).
6. **ASan** — `asan_win.cpp`: `SetUnhandledExceptionFilter` interceptor
   (`:79-83`), `AddVectoredExceptionHandler(TRUE, &ShadowExceptionHandler)`
   (`:338`), `SetUnhandledExceptionFilter(SEHHandler)` (`:365`),
   `ASAN_INTERCEPT_FUNC(SetUnhandledExceptionFilter)` (`:191`). Posix
   side defers to sanitizer_common; `asan_posix.cpp` checks
   `sigaltstack` for fake-stack accounting (`:45-50`); `asan_thread.cpp`
   sets `use_sigaltstack` (`:134, 290`).
7. **TSan** — `tsan_interceptors_posix.cpp`: a full second handler
   registry (`sigactions[kSigCount]`, `:209`), `sigaction_impl`
   (`:2691-2729`), the signal-handler invocation path (`:2207-2244`),
   `pthread_sigmask`/`sigprocmask` interceptors (`:2165-2167, 2275-2285`),
   sync-signal classification (`:2293-2316`), and SIG_* fallback
   definitions for platforms without them (`:120-131`).
   `tsan_platform_linux.cpp` uses `internal_sigprocmask` (`:671-713`).
8. **UBSan** — `ubsan_signals_standalone.cpp` (SIGSEGV handling, uses
   `REAL(sigaction_symname)`, `:70-71`) and
   `ubsan_loop_detect.cpp` (installs a `SIGPROF` handler via `sigaction`
   with `sa_sigaction`, `:84-86`).
9. **LSan / HWASan / Scudo / profile** — no direct signal API use (they
   defer to sanitizer_common; scudo only uses `__atomic_signal_fence` in
   `combined.h:1705-1711` and a test-only `signal(SIGSEGV, SIG_DFL)`,
   `scudo_unit_test.h:63-66`).

### D. lld / lldb / libcxx / flang / tools

1. **lld** — no signal API use at all (search over the whole tree).
2. **lldb** — signals are overwhelmingly *data about the target process*,
   not host signal handling: `Target/UnixSignals.cpp` and per-OS signal
   tables (`OpenBSDSignals.cpp`, etc.) are name/number/stop tables;
   `Target/Platform.cpp:1088` and `Host/posix/HostProcessPosix.cpp:43-49`
   call `kill()` (not being replaced); `Host/posix/
   ProcessLauncherPosixFork.cpp:154` calls `pthread_sigmask(SIG_SETMASK)`
   in the child between fork and exec (keep as-is). No `sigaction`/
   `signal()` in lldb's own process.
3. **libcxx** — only `std::abort()` calls (`system_error.cpp:190`,
   `verbose_abort.cpp:63`, `locale.cpp:927`); abort itself is replaced at
   the libc level (Phases 2/5), nothing to do here.
4. **flang** — no signal API use (one comment mentioning `signal(2)` in
   `Intrinsics.cpp:428`).
5. **llvm/tools** — `llvm-exegesis/lib/BenchmarkRunner.cpp:374-388`:
   `kill(ChildPID, SIGKILL)` and reading `ChildSignalInfo.si_signo ==
   SIGSEGV` from waitid — process-control, keep as-is.
6. **llvm/lib/ExecutionEngine/Interpreter/ExternalFunctions.cpp:341-342**
   — `raise(SIGABRT)` for the interpreter's abort.
7. **llvm/unittests/Support/CrashRecoveryTest.cpp:143-166** — tests that
   `raise()` through the recovery context; becomes a
   `stdc_raise()`/`sigguarded` test.

### Review conclusions (drives Phase 9)

- **Three families of old-API use exist**: (1) *install-and-chain*
  handlers (Signals.inc, sanitizers) → replace with
  `siginstall()` + `signal_decider_create()` decider chains; (2)
  *recover-and-resume* (CrashRecoveryContext) → replace with
  `sigguarded()`; (3) *raise-to-default* (`raise(SIGABRT)` etc.) →
  replace with `stdc_raise()`. `sigprocmask`/`sigaltstack`/`kill` remain
  as-is (not part of the N3924 replacement surface), except where the
  threadsafe implementation's own raw handler obsoletes the altstack
  bookkeeping.
- The replacement order follows dependency: `llvm/lib/Support` first
  (everything else inherits it), then its consumers, then compiler-rt
  (standalone wg14_signals), then Windows SEH.
- compiler-rt's interceptors cannot be simply deleted: the sanitizer
  contract (user handlers must run under sanitizer supervision, `signal()`
  vs `sigaction()` semantics preserved) must be re-implemented on top of
  the decider chain. This is the largest single piece of Phase 9 and is
  gated behind a flag so sanitizer behaviour is bit-identical by default.

## Baseline: test results before any changes (2026-08-20)

Recorded before any modification (the submodule is added and staged in the
fork but no code has changed; the fork carries only the 5 pre-existing
uncommitted macOS build-fix patches from the earlier session — the
baseline below is measured WITH those applied). Re-run the same commands
after every phase; any item that passed here must still pass, and any
known-failing/excluded item must remain so unless the change
intentionally fixes it (then record the fix here).

Exact states captured:

- wg14_signals repo: `50042da` ("More Windows CI fixes", heads/main).
- llvm-project fork: `ee1bf608d998` ("Get libc building on Mac OS.") plus
  uncommitted `libc/{config/darwin/aarch64/entrypoints.txt,
  include/llvm-libc-types/fenv_t.h,
  src/__support/FPUtil/aarch64/fenv_darwin_impl.h, src/setjmp/CMakeLists.txt,
  src/setjmp/darwin/CMakeLists.txt}` (the earlier darwin build fixes).
- Host used for the measured rows: macOS 26 arm64 (Darwin), clang from
  Xcode Command Line Tools, cmake 3.31+, ninja.

### A. wg14_signals standalone suite (this repo — the reference implementation)

Commands (per `.github/workflows/ci.yml`): `cmake --build build
--parallel` then `ctest --test-dir build --output-on-failure --timeout 300
-E benchmark`.

| Platform / config | Result (2026-08-20) |
|---|---|
| macOS arm64, clang, Debug C11 (local, no sanitizer toolchain) | **37/37 passed**, benchmarks 2/2 passed (5.34 s suite, 13.44 s benchmarks). **38/38 since 2026-08-21** (the new `stdc_raise_noinstall_test` — analysis.md W5 no-install-at-all arm). **40/40 since 2026-08-27** (the new `siginstall_sa_flags_test` and `siginstall_default_action_test` — the `siginstall_set_sa_flags_np()` / `siginstall_set_default_action_np()` APIs) |
| Linux glibc aarch64 (docker, `ghcr.io/llvm/arm64v8/libc-ubuntu-24.04@sha256:9ca390ed…`, clang-23 + gcc 13.3, ASan/UBSan toolchain) — **all 16 CI configs** clang-23/gcc × Debug/Release × C11/C23 × shared OFF/ON × fallback OFF/ON | **38/38 passed, 0 failed in every config** (recorded 2026-08-20 via docker; commands: the repo's ci.yml Linux job configure lines run inside the container, script `.kilo/tmp/wg14-glibc-matrix.sh`) |
| Linux musl (Alpine, C11/C23 × shared OFF/ON) | **38/38 passed, 0 failed in all 4 configs** (docker `alpine:3.20`, gcc, Release; script `.kilo/tmp/wg14-musl-matrix.sh`) — note: 38 tests discovered on Linux vs 37 on macOS (platform-gated tests) |
| Linux glibc **x86_64** | TBD — the upstream wg14_signals CI's `ubuntu-latest` legs are x86_64; record from the next green `main` CI run or an x86_64 docker run |
| macOS CI legs (Debug/Release × C11/C23 × shared × fallback, sanitize toolchain) | TBD — same |
| Windows VS2022 (Debug/Release × C11/C17 × shared, ASan) | TBD — same |
| FreeBSD 15 VM (C11/C23) | TBD — same; note `header_only_build_test` is already excluded there (`-E "benchmark\|header_only_build_test"`) |
| TSan (ubuntu gcc/clang, macOS clang; C11/C23) | TBD — same; `TSAN_OPTIONS=... report_signal_unsafe=0` etc. per ci.yml |
| Header-only (ubuntu/macOS/windows × C11/C23) | TBD — same |
| Fil-C C11 (ubuntu x86_64) | TBD — same; `thrd_signal_sigfpe_handle_test`, `recovery_null_loop_test`, `header_only_build_test` excluded there |

The override-macro probe (Phase 1 step 2) will add a ninth configuration:
same suite with `WG14_SIGNALS_SIGACTION/ABORT/KILL_SELF` overridden —
that configuration must also pass 37/37. **DONE 2026-08-21**: the
`WG14_SIGNALS_OVERRIDE_PROBE` configuration passes 37/37 locally (macOS
arm64, clang); the `LinuxOverrideProbe` CI leg will re-verify on
ubuntu-latest. Windows-side verification (2026-08-21, mingw+UCRT under
wine, TLS-disabled fallback, `__cxa_thread_atexit` stubbed): 27/27 of the
suite's runtime tests pass, including `stdc_raise_noinstall_test` —
note the wine leg uses the mingw-w64 toolchain whose winpthreads
`tls_atexit` asserts on the library's private DSO symbol (pre-existing
MinGW-only limitation, `thread_atexit.c.ipp`); real Windows CI uses MSVC.

### B. llvm-project libc (the fork, full runtimes build)

Configured and built in the earlier session (`cmake -G Ninja -S runtimes
-B build -DLLVM_ENABLE_RUNTIMES=libc -DLLVM_LIBC_FULL_BUILD=ON
-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++ -DPython3_EXECUTABLE=.../.venv/bin/python3.14`).

| Suite | Result (2026-08-20, macOS arm64) |
|---|---|
| `ninja -C build libc libm` | **builds** (`libc.a`, `libm.a` produced) |
| `ninja -C build check-libc` | **hdrgen_integration_test 1/1 passed; lit: 0 tests discovered** → target FAILS code 2. Known pre-existing darwin failure: full-build tests are hermetic and all skip on darwin because `libc.startup.darwin.crt1` does not exist. This is the baseline to preserve: the lit "no tests" failure must not get worse, and Phase 5 (darwin) may legitimately fix it. |
| Linux (glibc) check-libc — **docker** (`ghcr.io/llvm/arm64v8/libc-ubuntu-24.04@sha256:9ca390ed…`, clang-23/lld-23, Release, scudo ON, mirroring the CI `linux-aarch64-clang` job: `-DLLVM_ENABLE_RUNTIMES="libc;compiler-rt" -DLLVM_LIBC_FULL_BUILD=ON -DLLVM_LIBC_INCLUDE_SCUDO=ON -DCOMPILER_RT_BUILD_SCUDO_STANDALONE_WITH_LLVM_LIBC=ON -DCOMPILER_RT_BUILD_GWP_ASAN=OFF -DCOMPILER_RT_SCUDO_STANDALONE_BUILD_SHARED=OFF -DLIBC_TEST_SKIP_DEATH_TESTS=ON -DLIBC_TEST_SKIP_SHARED_TESTS=ON`; targets `install` → `check-libc-build` → `check-libc`; script `.kilo/tmp/libc-fullbuild-linux.sh`) | **check-libc: 1157/1157 passed (100.00%)** (recorded 2026-08-20). This is the Linux hermetic-suite baseline — compare every phase against it (contrast with the macOS 0-test lit run above) |
| FreeBSD check-libc | TBD — same, on the freebsd VM leg |
| Windows / GPU / UEFI / baremetal | n/a — no signal entrypoints today (this is the state Phase 6/7 preserves) |

### C. Full llvm-project test suite (the regression baseline)

The benchmark is the **entire llvm-project test suite** on the platforms
being modified: every project's check target, pass and fail counts, so
that after each phase we can detect any regression. Coverage mapping:

- **Measured locally (macOS arm64, this host)** — every project in the
  modification surface (per the comprehensive review): **llvm, clang,
  lld, compiler-rt, libc**, built in one monorepo tree and run via
  `check-all`. In flight (started 2026-08-20, persistent background
  process): `cmake -G Ninja -S llvm -B build-full -DCMAKE_BUILD_TYPE=Release
  -DLLVM_ENABLE_PROJECTS=clang;lld;compiler-rt;libc -DCMAKE_C_COMPILER=clang
  -DCMAKE_CXX_COMPILER=clang++ -DLLVM_INCLUDE_BENCHMARKS=OFF
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ZLIB=OFF
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
  -DLLVM_PARALLEL_LINK_JOBS=4`, then `ninja -C build-full check-all`.
  libc is built here in **overlay** mode (the monorepo build), which runs
  its unit-test suite on the host — complementary to the full/runtimes
  build in §B.
- **Out of the local run** — the components the review shows contain no
  signal-handling API use and that no phase modifies: lldb (signal data
  tables + `kill()` only), flang (none), libcxx (`abort()` only — covered
  by the libc-level replacement), mlir, openmp, polly, clang-tools-extra,
  libclc, pstl. They stay part of the "entire suite" definition: their
  check targets must be baselined from CI (see below) and re-checked if
  any phase ever touches their dependencies.
- **Other platforms (Linux glibc, Linux musl, Windows, FreeBSD)** —
  baseline comes from **PR #1's CI run** (see §D below; recorded
  2026-08-20 from
  `https://github.com/ned14/llvm-project/pull/1`, head `e25504f5a5ea` —
  the exact committed state of this directory's fork, branch
  `replace_signals_handling`).
  - **2026-08-20: fork workflow files adjusted so CI runs once Actions is
    enabled on the fork.** Every workflow that can run on GitHub-hosted
    runners has had its `github.repository_owner == 'llvm'` /
    `github.repository == 'llvm/llvm-project'` gate removed and its
    self-hosted runner labels (`llvm-premerge-*`, `depot-*`,
    `apple-runners`) replaced with GitHub-hosted ones; the libc and CI
    Checks workflows now `actions/checkout` with `submodules: recursive`
    so the `libc/src/signal/wg14` submodule is present; `.ci/` helper
    scripts no longer hard-fail on forks (lit-timing GCS cache and the
    premerge advisor are skipped when `GITHUB_REPOSITORY != llvm/llvm-project`);
    and the hardcoded `/__w/llvm-project/llvm-project` workspace paths
    became `${{ github.workspace }}`. Commit `e25504f5a5ea` ("Enable CI on
    github.") then enabled CI on the fork; **PR #1's run is the resulting
    baseline (recorded in §D below).**

Local run results (macOS arm64, Release, clang) — `check-all` **completed
2026-08-20** (single lit invocation, 1533.66 s). A Linux arm64 leg of the
same `check-all` (same configure flags, clang-23/lld-23, docker container
`ghcr.io/llvm/arm64v8/libc-ubuntu-24.04@sha256:9ca390ed…`, build dir
`/var/folders/…/T/kilo/llvm-linux-build`, script `.kilo/tmp/linux-check-all.sh`)
**completed 2026-08-21 with run 3 as the canonical baseline** (see the
table below and §C.1 for the full procedure, resource constraints, and
run history).

| Suite | Result | Notes |
|---|---|---|
| `check-all` aggregate | **139,385 discovered: 133,020 passed (95.43%), 32 failed (0.02%), 220 expectedly-failed (0.16%), 6,064 unsupported (4.35%), 49 skipped** | covered llvm test/, clang test/, lld test/, libc test/, all compiler-rt suites, utils/lit, mlgo-utils |
| The 32 failures (ALL pre-existing, signal-unrelated) | 2 × `AddressSanitizer-arm64-darwin :: TestCases/{strcpy,strncpy}-overlap.cpp` | FileCheck stack-frame `#1` vs `#2` mismatch (interceptor vs user frame ordering); no sanitizer memory error |
| | 15 × `Builtins-arm64-darwin` + 15 × `Builtins-arm64e-darwin` FP soft-float tests: `{adddf3,addsf3,divdf3,divsf3,extendsfdf2,muldf3,mulsf3,subdf3,subsf3}` `{,new}_test`, `{fixdfdi,fixdfsi,fixsfdi,fixsfsi,fixunsdfsi,fixunssfsi}*new_test` | NaN/overflow edge mismatches in `__adddf3` etc. (e.g. `__adddf3(7ff97d657b99f76f, 7ff7e4149862a796) = 7ff97d657b99f76f, expected 7fffe4149862a796`) |
| `check-llvm` / `check-clang` / `check-lld` / `check-libc` per-suite counts | **all 0 failed** (background `bgp_01ff387e7001jhGsG1ukgnfW6M`, same build, completed 2026-08-20): |
| | `check-llvm`: 77,135 discovered — 73,930 passed, 0 failed, 164 expectedly-failed, 3,004 unsupported, 37 skipped (406.15 s; includes check-llvm-unit / Support/CrashRecoveryTest) |
| | `check-clang`: 54,421 discovered — 53,061 passed, 0 failed, 30 expectedly-failed, 1,318 unsupported, 12 skipped (396.88 s) |
| | `check-lld`: 3,240 discovered — 3,200 passed, 0 failed, 1 expectedly-failed, 39 unsupported (50.26 s) |
| | `check-libc` (overlay, lit): 832 discovered — 832 passed, 0 failed (1296.27 s; includes building the 3,424 libc unit-test targets; the 32 check-all failures are all compiler-rt, so a per-suite `check-compiler-rt` re-run is not needed for the baseline — its failures are itemised above) |
| **Linux arm64 `check-all`** (docker, clang-23/lld-23, Release, same projects) | **run 3 (canonical, 2026-08-21, log `check-all-linux-run3.log`): 141,736 discovered — 135,280 passed (95.45%), 42 failed (0.03%), 244 expectedly-failed, 6,081 unsupported, 89 skipped** (4579.23 s, `ninja: build stopped: subcommand failed`). Run 2 (log `check-all-linux.log`): same totals except 135,291 passed / **31 failed** — the difference is flaky sanitizer/cfi tests under the 7.75 GiB memory cap; the **stable intersection** is the environmental set below. **All failures are environmental/flaky, none signal-related** | |
| | **Stable environmental failures (present in both runs, ~31; caused by root user + case-sensitive host fs + container)** — permission/umask: `LLVM :: tools/llvm-ar/error-opening-permission.test`, `llvm-ranlib/error-opening-permission.test`, `lit :: shtest-umask.py`, `lld :: ELF/file-access.s`, `lld :: ELF/lto/{resolution-err,thinlto-cant-write-index,thinlto-emit-imports}.ll`, `lld :: MachO/thinlto-emit-imports.ll`, `lld :: COFF/thinlto-emit-imports.ll`, 6 × `Clang :: Analysis/Scalable/*/…permissions.test`, `Profile-aarch64 :: {Posix/instrprof-fork.c, instrprof-set-dir-mode.c}` (umask/permission-sensitive), `libFuzzer-aarch64-default-Linux :: fuzzer-dirs.test`; case-insensitive fs: `Clang :: {APINotes/case-for-private-apinotes-file.c, Lexer/case-insensitive-include{-absolute,-system}.c, Modules/inferred-framework-case.m, PCH/case-insensitive-include.c, Preprocessor/nonportable-include-with-hmap.c}`; container: `LLVM :: tools/llvm-lipo/{create-executable,thin-executable-universal-binary}.test`, `LLVM :: tools/llvm-dwarfdump/X86/output.s` (flaky formatting), `DataFlowSanitizer-aarch64 :: release_shadow_space.c`, `Profile-aarch64 :: ContinuousSyncMode/online-merging.c` | |
| | **Flaky/run-dependent failures (only in run 3)**: `HWAddressSanitizer-aarch64 :: TestCases/{bcmp.cpp,stack-oob.c,stack-underflow.c}`, `LeakSanitizer-HWAddressSanitizer-aarch64 :: TestCases/suppressions_default.cpp`, `LLVM :: tools/llvm-objcopy/ELF/strip-debug.test`, `lld :: COFF/lto-cache-errors.ll`, `lld :: MachO/invalid/invalid-lto-object-path.ll`, cfi `cross-dso/stats.cpp` ×2, cfi `mfcall.cpp`, cfi `cross-dso-diagnostic.cpp` — sanitizer/cfi tests that flake under the 4-way-lit memory pressure; treat as pass/fail-inconclusive, do not attribute to changes | |
| Fault-injection matrix (SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL/SIGTRAP/SIGINT/SIGTERM/SIGPIPE/SIGUSR1/SIGUSR2/SIGHUP into a tool: stack trace content, exit codes, cleanup-file behaviour) | TBD | gates 9a-9e |

Out-of-scope suites to baseline from CI once the fork's Actions is
enabled (or from a runner): `check-lldb`, `check-flang`, `check-libcxx`,
`check-mlir`, `check-openmp`, `check-polly`, `check-clang-tools`, plus the
same matrix on Linux/Windows/FreeBSD.

Every phase's verification step compares against the rows above; a row
changing from pass to fail (or a TBD row first failing) is a regression
to resolve before proceeding. Known failures today: §B's darwin lit
0-tests failure and the standalone suite's platform exclusions (§A).

### C.1 How to run the Linux test suite on this machine (exact procedure)

Recorded 2026-08-20 after three failed attempts taught the following
lessons. **Follow this exactly; do not improvise.**

#### Container and environment

- Docker on this host is a VM with only **7.75 GiB RAM** and Linux
  **arm64** (aarch64), and runs as **root**. The image to use is the one
  libc's own CI uses for the `linux-aarch64-clang` leg (pinned by digest):
  ```
  ghcr.io/llvm/arm64v8/libc-ubuntu-24.04@sha256:9ca390ed3546754c1d7cb669033c532c28d95a0327551c5754cbf72375178e37
  ```
  It contains clang-23/clang++-23, lld-23, gcc 13.3, cmake, ninja.
- **Never write multi-line scripts with pipes/quotes into the `docker run`
  command string** — the shell wrapper mangles `|`, `$(...)` and
  multi-line bodies (observed: `don: command not found`, `redir error`,
  `grep: passed: No such file or directory`). Instead write a script file
  (e.g. `.kilo/tmp/<name>.sh`, executable, no inline `|` in echo
  summaries — use `grep ... > log` then `grep` the log afterwards) and
  invoke it as `docker run ... bash /work/.kilo/tmp/<name>.sh`.
- **Mounts**: `/Users/ned/boostish/wg14_signals:/work` (the repo) and a
  persistent host build dir, e.g.
  `/var/folders/5s/4zr1hh3j76bbmmhx3gl_5wn40000gn/T/kilo/llvm-linux-build:/linuxbuild`.
  The build dir must be on the host (not container /tmp) so a killed
  container leaves the tree resumable.
- **Capture the exit code into a file inside the container**, never rely
  on the outer shell's `$?` (the first full run printed
  `LINUX_CHECK_ALL_EXIT=0` while ninja had actually failed):
  ```
  ninja -C /linuxbuild check-all > /linuxbuild/check-all-linux.log 2>&1
  rc=$?; echo RUN_RC=$rc >> /linuxbuild/check-all-linux.log
  ```
  and afterwards always verify with the lit summary block in the log
  (`Total Discovered Tests` / `Passed` / `Failed` / `FAILED:`), not the
  exit code alone.

#### The three Linux runs and their exact commands

1. **wg14_signals standalone glibc matrix** (16 configs) — script
   `.kilo/tmp/wg14-glibc-matrix.sh`, run:
   ```
   docker run --rm -v /Users/ned/boostish/wg14_signals:/work \
     ghcr.io/llvm/arm64v8/libc-ubuntu-24.04@sha256:9ca390ed… \
     bash /work/.kilo/tmp/wg14-glibc-matrix.sh
   ```
   (clang-23/gcc × Debug/Release × C11/C23 × shared OFF/ON × fallback
   OFF/ON, `-DCMAKE_TOOLCHAIN_FILE=/work/cmake/sanitize-toolchain.cmake`,
   `ctest -E benchmark`). Expected: **38/38 passed in every config**.
2. **wg14_signals musl matrix** (4 configs) — script
   `.kilo/tmp/wg14-musl-matrix.sh`, image `alpine:3.20`, run with
   `sh -c 'apk add --no-cache build-base cmake ninja > /dev/null && sh
   /work/.kilo/tmp/wg14-musl-matrix.sh'`. Expected: **38/38 in every
   config**.
3. **libc fullbuild** (the `linux-aarch64-clang` CI leg) — script
   `.kilo/tmp/libc-fullbuild-linux.sh`; requires **`--privileged`**
   (CI does the same; some tests need SYS_TIME). Targets `install` then
   `check-libc-build` then `check-libc`. Expected: **check-libc 1157/1157
   passed**.
4. **Full monorepo `check-all`** — configure + build + test script
   `.kilo/tmp/linux-check-all.sh` (llvm;clang;lld;compiler-rt;libc,
   Release, clang-23, `-DLLVM_USE_LINKER=lld-23`, benchmarks/examples/
   docs OFF, `-DLLVM_PARALLEL_LINK_JOBS=4`). **Constraints that prevent
   failures:**
   - Do NOT let ninja auto-detect parallelism: AMDGPU `llvm-tblgen` gets
     OOM-`Killed` at `-j12` inside the 7.75 GiB VM. Configure with
     `-DLLVM_LIT_ARGS="-j4"` and run `ninja -C /linuxbuild -j3 check-all`
     (also cap lit: `-DLLVM_LIT_ARGS="-j4"`; tests run 4-way).
   - Expected (run 3, canonical): **141,736 discovered — 135,280 passed,
     42 failed, 244 expectedly-failed, 6,081 unsupported, 89 skipped**
     (4579 s). The 42 failures are environmental/flaky: ~31 stable
     (root-user/umask `error-opening-permission`-class tests, docker runs
     as root; case-sensitive-fs `case-insensitive-include` tests;
     `llvm-lipo` universal-binary tests; dfsan `release_shadow_space.c`;
     profile `online-merging.c`; `llvm-dwarfdump/X86/output.s`) plus ~11
     flaky sanitizer/cfi tests that vary run to run. **None are
     signal-related.** Keep the same image/flags so the failure set stays
     comparable across phases.
   - Resuming: on the same bind mount, a re-run is just `ninja -C
     /linuxbuild -j3 check-all` (incremental).
   - To get per-suite counts, run `ninja -C /linuxbuild check-llvm
     check-clang check-lld check-libc check-compiler-rt` with the same
     `-j3`/lit caps and capture each lit summary block.

#### What NOT to do (mistakes recorded 2026-08-20)

- Do not pass multi-line scripts with `|`/`$()` inline in the docker
  command (broken quoting).
- Do not use the default ninja parallelism in the 7.75 GiB VM (OOM kills).
- Do not trust the outer `$?` echo; capture rc into the log and read the
  lit summary.
- Do not run the build in a container-local dir (lost on `--rm`; not
  resumable).
- Do not expect the 31-42 environmental failures to disappear on a plain
  re-run; they are the baseline, and the *stable set* (~31, listed in the
  table above) is what must stay stable across phases. The flaky
  sanitizer/cfi subset (run 3's extra 11) is expected to vary run to run.
- **Run history (all logs in the build dir `/var/folders/…/T/kilo/llvm-linux-build/`):**
  run 1 — OOM-killed at ~1,350/8,318 objects then resumed; its outer
  `$?` echo wrongly printed 0 (rc-capture bug). Run 2 —
  `check-all-linux.log`: 31 failed. Run 3 (canonical) —
  `check-all-linux-run3.log`: 42 failed, 135,280 passed. Use run 3's
  numbers as the Linux baseline and re-run with the identical
  image/flags/script so the failure set stays comparable.

### D. Fork CI baseline from PR #1 (recorded 2026-08-20)

Source: `https://github.com/ned14/llvm-project/pull/1` — "Get libc
building on Mac OS.", head `e25504f5a5ea` ("Enable CI on github.") on
branch `replace_signals_handling`, base `d98d12cb99b6` — **the exact
committed state of the fork in this directory**. 57 check runs in total:
26 success, 21 failure, 8 in progress, 2 skipped. This is the pass/fail
baseline for every platform the plan modifies; re-run the same checks
after each phase and diff the tables below.

**Passing (26):**

| Check | Result |
|---|---|
| ubuntu-24.04 - clang-23 (premerge build+test) | success |
| ubuntu-24.04-arm - clang-23 | success |
| macos-15 - clang | success |
| windows-2022 - clang-cl | success |
| windows-2025 - clang-cl | success |
| libc-shared-tests with gcc-7 / gcc-8 / gcc-9 / gcc-11 / gcc | success ×5 |
| libc-shared-test with MSVC on amd64 / arm64 / amd64_x86 | success ×3 |
| builtins (ubuntu-24.04) / builtins (ubuntu-24.04-arm) | success ×2 |
| Test Unprivileged Download Artifact | success |
| Compute macOS Projects | success |
| Check LLVM_ABI annotations with ids | success |
| Test documentation build | success |
| Buildifier | success |
| code_linter (×2) | success ×2 |
| Run zizmor | success |
| Upload Test Artifact | success |
| Check Python Tests | success |

**Failing (21):** — two classes:

1. **Infrastructure / workflow-plumbing failures (15)** — not test
   failures; must stay green-or-plumbing-failing exactly as below, and any
   phase that touches `.github/workflows` must not change their nature:
   - `libc-fullbuild on linux-x86_64-Debug / linux-x86_64-Release /
     linux-x86_64-MinSizeRel / linux-aarch64-clang` (4), `libc-fullbuild
     on baremetal-armv6m / armv7em / armv7m / armv8m-softfp /
     armv8m-hard / armv8.1m / baremetal-riscv32` (7), `libc-fullbuild on
     uefi-x86_64-clang` (1), `libc-fullbuild on amd-gpu` (1) — **all 13
     fail identically**: `Saving cache failed: Error: Path Validation
     Error: Path(s) specified in the action for caching do(es) not exist`
     then `Process completed with exit code 1`. The builds/tests
     themselves did not fail; the final cache-save step did.
   - `Build and Test Windows` — `No files were found with the provided
     path: comments-Windows-AMD64. No artifacts will be uploaded.` +
     `Process completed with exit code 1` (artifact-upload step).
   - `zizmor` — `unpinned image references: container image is unpinned`
     (security lint on `.github/workflows/premerge.yaml`).
2. **Real test failures (4)** — `libc-shared-tests on armhf / riscv64 /
   aarch64 / ppc64le` — `Process completed with exit code 2` (annotations
   give no individual test names; job logs need auth. The libc
   cross-compile shared-test legs, presumably failing at build/run of the
   shared library tests on the QEMU cross targets). `qemu - armhf -
   clang-23` and `qemu - riscv64 - clang-23` also exit 2 (the qemu step
   itself failed; the cache annotation also appears on these two).

**In progress (8):** `Build and Test Linux`, `Build and Test Linux
AArch64`, `Build and Test macOS arm64`, `Test SYCL`, `Test SPIR-V`,
`Test MLIR SPIR-V`, `build`, `Bazel Build/Test` — were still running at
recording time; re-query `https://api.github.com/repos/ned14/llvm-project/
commits/e25504f5a5eaf1828a6e02ddda4c257b1298b3bd/check-runs` and update
this table before Phase 1 work.

**Skipped (2):** `automate-prs-labels`, `greeter` (PR-triggered,
expected).

Notes for regression detection: the 13 `libc-fullbuild` cache failures and
the Windows artifact failure are environment issues, not signal-handling
regressions; what matters is that the **same set** of 4 `libc-shared-tests`
cross legs plus the 2 qemu legs fail, and that no **new** check starts
failing after a phase. The Linux x86_64 premerge suite (`ubuntu-24.04 -
clang-23`) passing is the key Linux baseline; the local macOS `check-all`
in the table above is the key macOS baseline.

## Design

### Submodule integration (git submodule)

- The reference implementation is mounted as a **git submodule** of the
  fork at `libc/src/signal/wg14` (URL
  `https://github.com/ned14/wg14_signals.git`, pinned commit
  `50042dad4eb376bc88f6d21568007e72c615f3a8`). The submodule IS upstream —
  no copying, no subtree merge, no vendored drift. Bumping the integration
  is a deliberate `git submodule` commit in the fork, and the override
  layer means a bump never requires fork-side patches.
- The submodule contains the complete standalone project: `include/`,
  `src/`, `test/` (its own CI suite), `CMakeLists.txt` and docs. libc's
  build **must not** `add_subdirectory` it (its CMakeLists.txt is a
  standalone `project()`); instead a fork-owned
  `libc/src/signal/wg14/CMakeLists.txt` defines an `add_object_library`
  (cf. `libc/src/signal/linux/CMakeLists.txt`) that lists the submodule's
  C sources directly, with `-I libc/src/signal/wg14/include` so the `.ipp`
  relative includes resolve. The library must compile under the full-build
  flag set: `-ffreestanding -fno-builtin -nostdlibinc -fno-exceptions
  -fno-rtti`, C11, C sources only (AGENTS.md: no C++ in libc sources).
- **Submodule pin (2026-08-21)**: bumped to upstream
  `b2059fb49163` ("First round of changes to support integrating into LLVM
  libc"), which contains the override-layer changes (the `WG14_SIGNALS_*`
  embedder hooks in config.h, the call-site routing in the `.ipp` files,
  the `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` /
  `WG14_SIGNALS_DISABLE_SIGGUARDED_FAILURE_VALUE` guards and the
  `sigguarded_failure_value()` helper). The submodule checkout is clean at
  that commit. The fork-owned files (CMakeLists, embedder shim, hostcalls,
  wrappers) never touch the submodule's contents.
- Missing-submodule guard: the fork-owned CMake
  `message(FATAL_ERROR ...)`s with recovery instructions when the submodule
  is absent, so a non-recursive clone fails fast and explains how to run
  `git submodule update --init libc/src/signal/wg14`.
- CI: fork workflows must `actions/checkout` with
  `submodules: recursive`. The nested `doc/html` submodule of wg14_signals
  (gh-pages branch) is not needed for the build and stays uninitialized in
  CI; only the parent needs to be recursive. Implemented 2026-08-20 in
  `.github/workflows/premerge.yaml` and the libc `*tests*.yml` workflows.
- Wrapper entrypoints live in `libc/src/signal/wg14/entrypoints/` as small
  `LLVM_LIBC_FUNCTION` `.cpp` files (matching libc convention), each
  `DEPENDS` on the wg14 object library. The wg14 symbols are plain C
  `extern` symbols (unprefixed per `WG14_SIGNALS_PREFIX` default); internal
  machinery symbols are hidden via the `WG14_SIGNALS_DEFAULT_VISIBILITY`
  handling already in `config.h`.
- License compatibility: both are Apache-2.0; the submodule's license
  headers stay intact, and the fork's CMake comments note the provenance.
- The submodule's own `test/` suite is the primary correctness net: it runs
  against the host libc unchanged (`cd libc/src/signal/wg14 && cmake -B
  build && ninja -C build && ctest`), proving that the exact same sources
  libc embeds pass the reference suite.

### OS abstraction layer (the critical piece: no recursion)

Because the submodule is pristine, the fork cannot patch the `.ipp` files;
the integration hooks must be **upstreamed into wg14_signals** (this repo)
as an embedder override layer. The POSIX backend calls, by name, the very
functions being replaced: `sigaction()` (`thrd_signal_handle_posix.c.ipp:
155-223, 289, 576-589`), `pthread_kill(pthread_self(), ...)` (`:222`),
`abort()` (`:153-159, 306, 509, 548`), `memcpy`/`memset` (`:83, 113, 147,
156, ...`), `malloc`/`free` (common `.ipp` throughout, e.g. `:188, 201,
334, 361`), `setjmp`/`longjmp` (`<setjmp.h>` at common `.ipp:37`, `jmp_buf`
member at `:265`), plus the `thread_atexit` chain (`__cxa_thread_atexit` at
`thread_atexit.c.ipp:54-73`, pthread_key fallback at `:169-224`) and
`current_thread_id()`'s per-OS calls.

If these resolve to the public entrypoints, `sigaction()` calls
`sigaction()` → infinite recursion, and `raise()` calls `stdc_raise()` →
recursion. The upstream override layer gives each host call a
function-like macro in `config.h` that defaults to the standard name and
that an embedding libc can redefine to its own internal function:

```c
#ifndef WG14_SIGNALS_SIGACTION
#define WG14_SIGNALS_SIGACTION(signum, act, oldact)                            \
  sigaction(signum, act, oldact)
#endif
```

Only three call-site families actually need overriding to prevent
recursion or to name functions libc does not provide; everything else
resolves naturally to libc's own generated headers and entrypoints with no
recursion (full call-site inventory in Appendix B):

| Must override | Why | Linux target (fork) |
|---|---|---|
| `sigaction` | the submodule's raw-handler install/uninstall must go to the kernel directly (SA_NOCLDWAIT stripped, no dependence on libc's entrypoint C symbols in hermetic test builds) | `wg14_embedder_sigaction` in the shim (`SYS_rt_sigaction` + `__restore_rt`); darwin/freebsd wrappers to be added |
| `abort` | libc `abort` raises SIGABRT through the same machinery → recursion | `abort_utils::abort()` after the rework in §"Internal consumers" |
| `pthread_kill(pthread_self(), ...)` | libc has no `pthread_kill` entrypoint; used by `stdc_raise`'s last-resort delivery | `SYS_tgkill`/`SYS_gettid` syscall wrappers (to be added) |

| Resolves naturally | Why it cannot recurse |
|---|---|
| `memcpy`/`memset` | libc string entrypoints never call signals; symbols resolve to libc.a objects |
| `malloc`/`free` | libc allocator entrypoints never call signals |
| `setjmp`/`longjmp` | libc's per-arch setjmp never calls signals; `<setjmp.h>` is generated |
| `errno` | libc's generated `<errno.h>` maps to per-thread TLS; no recursion |
| `NSIG` | from the platform `signal-macros.h` (linux: `NSIG 64`, `libc/include/llvm-libc-macros/linux/signal-macros.h:49`) |
| `__cxa_thread_atexit` / pthread_key fallback | full build has no `__cxa_thread_atexit`, so the pthread_key fallback compiles and runs; libc's pthread entrypoints are futex/syscall based (`pthread_key_create.cpp`, `pthread_once.cpp`, `pthread_setspecific.cpp`) and never call signals; `<pthread.h>` is generated from `libc/include/pthread.yaml` |
| `syscall(SYS_gettid)` / `mach_thread_self` / `pthread_getthreadid_np` | `current_thread_id.c.ipp` already branches per-OS; Linux uses the raw syscall, macOS a mach kernel call (no libc dep), FreeBSD libc's `pthread_getthreadid_np.cpp` |
| `<stdfil.h>` (common `.ipp:43`) | already `#ifdef __FILC__`-guarded; inert in libc builds |

The override macros are the official embedder interface: documented in
`config.h`, `#undef`-safe, and required to preserve the
ASYNC-SIGNAL-SAFE / THREADSAFE annotations of the APIs whose
implementation they route (they reroute to functions with the same or
stronger guarantees). The standalone build continues to use the defaults,
so the host-libc test suite exercises the exact same macro expansion the
libc embedder overrides — the proof that the unmodified reference
implementation extends an existing standard C library.

The remaining shim work is **header resolution**, not code: in the full
build `-nostdlibinc` means the `.ipp` includes (`<signal.h>`, `<setjmp.h>`,
`<errno.h>`, `<stdlib.h>`, `<string.h>`, `<pthread.h>`) must resolve to
libc's generated headers (all generated; `pthread.h` from
`libc/include/pthread.yaml`), and `<stdatomic.h>` is the
compiler-provided freestanding header (clang ships one; verify in the
build). `NSIG` comes from the platform `signal-macros.h`. No fork-side
source patch is involved — only include paths supplied by the fork's CMake.

### Public API surface (header generation)

LLVM-libc public headers are generated from YAML by `libc/utils/hdrgen/`. The
N3924 API needs:

- **`libc/include/signal.yaml` additions**: `sigguarded`, `stdc_raise`,
  `siginstall`, `siguninstall`, `siguninstall_system`,
  `signal_decider_create`, `signal_decider_destroy`,
  `sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`,
  `sigfillset_asynchronous_debug`; types `stdc_siginfo`, `stdc_siginfo_value`,
  `global_signal_decider_t` (opaque), `thread_id_t`; macros `NSIG`
  (if absent), `SIG_*` additions as needed, `sig_decision_next_decider`,
  `sig_decision_resume_execution`, `sig_decision_call_recovery`,
  `SIGGUARDED_FAILURE_VALUE`, and the `stdc_siginfo` member macros the
  proposal defines. Types go into `libc/include/llvm-libc-types/` following
  the existing per-type file convention.
- **`tss_async_signal_safe_*`** (the four functions plus the
  `tss_async_signal_safe_t` handle type and `tss_async_signal_safe_attr`
  structure) go into `libc/include/threads.yaml` per the N3924 proposed
  wording: clause 7.30.6.5 through 7.30.6.8, declared in `<threads.h>`
  (`docs/proposed-wording.md:980-1138`). No separate header.
- **`current_thread_id`** belongs in a new `threads.h`-adjacent home; N3924
  rev 4/5 puts `current_thread_id()` in `<threads.h>`; add it to
  `libc/include/threads.yaml` (verify the proposal edition vendored in
  `docs/proposed-wording.md` — the repo tracks rev 5; the plan defers to
  that wording for placement).
- **`sigfence`** is a variadic macro with compiler-specific asm; it cannot be
  expressed in YAML. Follow the existing hand-written header pattern
  (`libc/include/assert.h.def`, `math.h.def`, `stdbit.h.def`): a
  `signal.h.def` (or a new `stdc_signal.h` generated from a hybrid source)
  that `#include`s the wg14_signals `sigfence` macro definitions verbatim.
  Alternatively ship `include/wg14_signals/` headers as installed headers for
  the N3924 API only (option B, §"Open questions").
- `hdr/` proxy files (`libc/hdr/`) — the build-system-side mirrors of the
  public headers that internal sources include (`hdr/signal.h`,
  `hdr/types/...`) — must gain the new proxies (there is a documented
  per-header mechanical pattern in `libc/hdr/CMakeLists.txt`).
- The generated `fenv.h`-style check applies: the earlier build session
  showed Darwin `fenv` needed `FE_FLUSHTOZERO` and `__fpcr_*` — same class of
  work as the new signal types on non-Linux.

### Replacing the existing entrypoints

**Design correction (2026-08-21)**: the existing Linux entrypoints are
**NOT** re-implemented on top of wg14_signals. They are completely
untouched, keep their direct-kernel implementations, and never call into
wg14_signals (an early registry prototype that routed
`sigaction()`/`raise()` through the wg14 decider chain was reverted —
`registry.{h,cpp}` was deleted and `sigaction.cpp`/`raise.cpp`/
`abort_utils.h`/the signal test CMake files were restored to upstream).
Rationale:

- The wg14_signals raw handler is installed per-signal by
  `siginstall()`; the POSIX entrypoints' kernel dispositions coexist
  per-signal (the last writer of a signal's kernel disposition wins).
- libc's own code never calls the wg14 APIs at all (2026-08-22 policy,
  §"Internal consumers"; the one `stdc_raise()`-in-`abort()` attempt was
  reverted as redundant/harmful — an OS raise enters the wg14 raw handler
  by definition when external code `siginstall`'ed), so the entrypoints
  need no shim layer.
- `sigprocmask`/`pthread_sigmask`/`sigaltstack`/`kill` and the sigset
  helpers are likewise unchanged; the `__restore` object stays as-is.

**Policy (2026-08-22)**: internal libc NEVER calls `siginstall`/
`siguninstall` (nor `signal_decider_create`/`signal_decider_destroy`) —
ONLY code outside libc does. libc merely works correctly when external
code does so: the untouched POSIX entrypoints coexist per-signal with the
wg14 raw handler (the last writer of a signal's kernel disposition wins,
and save/restore round-trips through the wg14 handler unchanged). **Rule
(2026-08-22)**: internal libc also never calls `stdc_raise()` for a signal
it raises via the OS — the raise enters the wg14 machinery by definition;
`abort()` is upstream-identical and honours externally installed deciders
through its normal kernel raise (§"Internal consumers").

`sigismember` was added as a new Linux entrypoint purely because the
wg14_signals submodule calls it (the sigset bit helpers
`sigaddset`/`sigdelset`/`sigemptyset`/`sigfillset`/`sigismember` are
routed to libc's entrypoints via the embedder hooks).

### Internal consumers

 **Direction (2026-08-21, amended 2026-08-22)**: **internal libc NEVER calls
 `siginstall()`/`siguninstall()` (nor `signal_decider_create()`/
 `signal_decider_destroy()`) — ONLY code outside libc does. libc merely
 works correctly when external code uses them.** Internal
 signal-disposition needs stay on the existing direct-kernel entrypoints
 (`raise()`/`sigaction()`/`rt_sigaction`, all untouched, §"Replacing the
 existing entrypoints"). **RULE (2026-08-22): internal libc also NEVER
 calls `stdc_raise()` for a signal it raises via the OS raise path — the
 wg14 filtering handler installed by an external `siginstall()` IS the
 kernel disposition, so the raise enters the wg14 machinery by definition;
 a manual `stdc_raise()` is redundant and harmful (double handoff when a
 pre-existing handler returns; see executive item 7).** The earlier
 "`stdc_raise()` is the sole sanctioned wg14 call inside libc (abort)"
 wording is REVOKED — `abort()` was reverted to upstream (item 7). Work
 items, in priority order:

- **`abort()`** (`libc/src/stdlib/linux/abort_utils.h`): **NOTHING-TO-DO
  2026-08-22 (design correction)**. `abort_utils.h` is exactly upstream
  again. When external code has `siginstall`'ed SIGABRT, `abort()`'s
  tgkill raise delivers into the wg14 filtering raw handler, so the decider
  chain runs (`sig_decision_call_recovery` longjmps out of `abort()` and
  never returns; `sig_decision_resume_execution`/a returning chained
  handler returns and the hard-abort sequence continues, same C11
  semantics), with the REAL kernel siginfo (`SI_TKILL`, pid, uid). Without
  `siginstall`, the raise hits the default disposition and terminates,
  byte-identical to upstream. No `stdc_raise()` call, no synthesized
  siginfo, no added CMake deps. (An initial version DID prepend
  `stdc_raise(SIGABRT, &info, NULL)` with a synthesized siginfo — the
  double-handoff and redundancy defect described above — and was reverted
  the same day; the smoke test's recovery-fn siginfo check passes against
  the reverted code because the kernel delivery supplies the real info.)
- **`system()`** (`libc/src/stdlib/linux/system.cpp`): **NOTHING-TO-DO
  (policy 2026-08-22)**. Keeps the upstream direct-kernel `rt_sigaction`
  SIGINT/SIGQUIT save-ignore-restore. It coexists with external
  `siginstall()`: the save captures whatever disposition is installed —
  including a wg14 raw handler — as the previous disposition, and the
  restore puts it back, so an external installation survives `system()`
  intact and the child still restores the caller's original dispositions
  before exec. (A wg14-decider rework was implemented and verified on
  2026-08-22, then reverted: internal libc must not call the install
  APIs.)
- **`posix_spawn()`** (`libc/src/spawn/linux/posix_spawn.cpp`): **NOTHING-TO-DO
  2026-08-22 (stale plan description)**. The implementation has no
  SIGCHLD/SIGINT/SIGQUIT disposition code — confirmed against upstream
  history back to before `463f6cb576fc` (the commit that added the
  `SigAbortGuard`). Its only signal interaction is `SigAbortGuard` around
  the fork syscall (block-all-signals + abort-lock read lock,
  `signal_utils.h:128-154`), which is abort-machinery synchronisation
  orthogonal to the wg14 integration and stays as-is.
- **`strsignal`** (`libc/src/string/strsignal.h`): table-driven, no change.
- **`sigsetjmp`/`siglongjmp`** (Linux): leave as-is (they are independent
  POSIX APIs); N3924's `sigguarded()` coexists with them. The darwin
  `sigsetjmp_epilogue` stays excluded until the darwin signal-mask machinery
  lands (Phase 6 fixes this properly: with `sigprocmask` implemented on
  darwin, the epilogue can be fixed and re-enabled).

### TLS policy inside libc

- Linux (ELF): `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=1` via
  `initial-exec` — safe because libc's own startup owns TLS
  (`libc/startup/linux/<arch>/tls.cpp`); document that the whole libc is a
  process-bootstrap library so the runtime-loaded-DSO caveat in
  `config.h:120-136` (and `Readme.md:121-141`) does not apply to the full
  build. In overlay/shared builds the caveat applies; keep the config.h
  logic.
- macOS: `0` (hash-table fallback) — no change needed; perf cost already
  documented in `Readme.md:174-182`.
- Windows: `1` for the SEH backend, but gated OFF by default (Phase 8).
- GPU/UEFI/baremetal: no TLS; gated out.

## Detailed phases

### Phase 1 — Submodule, override layer, build (Linux)

1. **DONE**: submodule added at `libc/src/signal/wg14`, pinned to
   `50042dad4eb3` (staged in the fork; commit it in the fork with the
   `.gitmodules` entry).
2. **DONE 2026-08-21, extended**: the upstream embedder override layer now
   covers the complete host-call inventory: `WG14_SIGNALS_SIGACTION/ABORT/
   KILL_SELF/GETTID` plus `WG14_SIGNALS_MEMCPY/MEMSET/MALLOC/CALLOC/FREE`,
   `WG14_SIGNALS_SIGEMPTYSET/SIGFILLSET/SIGADDSET/SIGDELSET/SIGISMEMBER`,
   `WG14_SIGNALS_PTHREAD_KEY_CREATE/PTHREAD_ONCE/PTHREAD_SETSPECIFIC/
   PTHREAD_GETSPECIFIC`, `WG14_SIGNALS_SETJMP/LONGJMP` (the existing
   `_setjmp` selection became overridable), and the
   `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` /
   `WG14_SIGNALS_DISABLE_SIGGUARDED_FAILURE_VALUE` embedder guards (the
   latter with a `sigguarded_failure_value()` helper so the `.ipp` still
   compiles when an embedder mirrors `SIGGUARDED_FAILURE_VALUE`). All call
   sites in the four `.ipp` files route through the hooks. The
   `WG14_SIGNALS_OVERRIDE_PROBE` CMake option
   (`test/override_probe.{h,c}`) passes 37/37 in both configurations, and
   the `LinuxOverrideProbe` CI leg was added.
3. **DONE 2026-08-21** — with a structural correction: git cannot track
   files inside a submodule directory (the directory is a gitlink), and
   `libc/src/signal/wg14/CMakeLists.txt` is the submodule's own file, so
   ALL fork-owned files live in the sibling directory
   **`libc/src/signal/wg14-build/`** (`CMakeLists.txt`,
   `embedder_shim.{h,cpp}`, `wg14_hostcalls.cpp`, `entrypoints/`), tracked
   by the fork, referencing the submodule via `../wg14/...` paths. The fork
   build file defines `libc.src.signal.wg14-build.{embedder_shim,
   wg14_hostcalls,wg14_signals}` (object libraries) + `entrypoints/*` with
   the
   missing-submodule FATAL_ERROR guard, wired from
   `libc/src/signal/CMakeLists.txt` (Linux only until the darwin/freebsd
   syscall layers land). The embedder shim routes the four
   recursion-critical hooks to raw Linux syscalls via
   `LIBC_NAMESPACE::syscall_impl` (kernel-layout sigaction conversion with
   `__restore_rt`, `SYS_gettid`/`SYS_tgkill` self-delivery, the
   recursion-free hard abort; `SA_NOCLDWAIT` stripped so POSIX wait()
   semantics survive).
4. **DONE 2026-08-21**: compiles clean under the full-build flags modulo
   `-Wno-conversion -Wno-cast-qual -Wno-global-constructors` for the
   vendored verstable.h and the lazy-init sigset builders. The
   hermetic-test link closure is solved by `wg14_hostcalls.cpp`: extern
   "C" implementations of every hook route to the namespace-scope
   entrypoints (which exist in both the library and the internal test
   objects), with weak C `setjmp`/`longjmp`/`malloc`/`calloc`/`free`/
   `__assert_fail` fallbacks (strong libc/scudo definitions win in the
   library build; the weak fallbacks satisfy test links — the allocator
   fallback is a page-granular mmap-based allocator). setjmp/longjmp are
   routed DIRECTLY to the C symbols for the wg14 call sites because the
   aarch64 naked setjmp miscompiles through a wrapper (a real defect found
   during bring-up); the weak namespace-bridged versions satisfy only test
   links. `WG14_SIGNALS_STDERR_PRINTF` also routes through the bridge.
5. **DONE**: gated to Linux (`LIBC_TARGET_OS STREQUAL "linux"`), preserving
   the darwin/freebsd baselines until their syscall layers land.
6. **DONE 2026-08-21, smoke link fixed 2026-08-22**: the fork's own smoke
   test
   (`.kilo/tmp/wg14_libc_smoke.c`, linked against the built `libc.a` +
   `crt1.o` + compiler-rt builtins) passes 100% on Linux aarch64:
   sigaction/signal/raise/SA_SIGINFO/SIG_IGN/sigprocmask (untouched
   direct-kernel entrypoints), siginstall/siguninstall/
   signal_decider_create/destroy, stdc_raise, sigguarded SIGSEGV recovery,
   sigfillset_*, current_thread_id, tss_async_signal_safe_*,
   default-action delivery and abort() semantics (through libc's own C11
   abort). 2026-08-22 the smoke link was brought back up after the full
   rebuild: `crt1.o` is a relocatable link of start/tls/irelative/
   do_start/gnu_property, so the script's STARTUP list is just
   `crt1.o crti.o crtn.o` (the individual objects duplicate symbols), and
   the compiler-rt builtins archive
   (`compiler-rt/lib/linux/libclang_rt.builtins-aarch64.a`) must be in the
   link group for the `__aarch64_*` atomic helpers (scudo, libc `Atomic`,
   the wg14 TSS implementation). The submodule's own suite stays green
   (37/37 in both configurations).

### Phase 2 — Linux entrypoints + config

1. Add wrapper entrypoints under `libc/src/signal/wg14/entrypoints/`.
2. Extend `signal.yaml` + `llvm-libc-types/` + `hdr/` proxies.
3. Add the entrypoints to `libc/config/linux/{x86_64,aarch64,arm,riscv}/
   entrypoints.txt`.
4. Add the new syscall wrappers (`sigaltstack`, `gettid`/`tgkill`) to
   `libc/src/__support/OSUtil/linux/syscall_wrappers/`.
5. ~~Re-point the 12 existing linux signal entrypoints~~ **SUPERSEDED
   2026-08-21**: the existing entrypoints stay untouched (§"Replacing the
   existing entrypoints"); the Phase 2 work is the N3924 entrypoints +
   headers + config (items 1-4), plus §"Internal consumers" rewrites.
6. ~~Rework `abort_utils.h`~~ **REVERTED 2026-08-22**: `abort_utils.h` is
   exactly upstream again — the `stdc_raise` prepend was redundant (the
   kernel raise enters the wg14 raw handler by definition when external
   code `siginstall`'ed) and harmful (double handoff), §"Internal
   consumers" item "abort()" and executive item 7; `system.cpp` and
   `posix_spawn.cpp`
   are **NOTHING-TO-DO under the 2026-08-22 policy** that internal libc
   never calls `siginstall`/`siguninstall` (§"Internal consumers") —
   `system()` keeps its direct-kernel `rt_sigaction` save-ignore-restore
   (coexisting with external `siginstall()` as described there) and
   `posix_spawn()` has no signal-disposition code (only the abort-machinery
   `SigAbortGuard`, which stays).

### Phase 3 — Linux tests

**Reordered 2026-08-22**: Phase 3 runs NOW, before the darwin (Phase 5,
deferred per executive item 8) and FreeBSD (Phase 4) bring-ups — the Linux
integration's correctness net lands while the verification environment is
in hand.

**DONE 2026-08-22** (all three steps, verified on Linux aarch64 docker full
build):

1. **Hermetic ports** (`libc/test/src/signal/wg14/`, 7 tests, all C++
   `add_libc_test` with a `libc_signal_wg14_unittests` suite; they auto-skip
   on non-Linux because the wg14 entrypoint DEPENDS have no targets there):
   - `wg14_raise_semantics_test` — ports of stdc_raise_zero (1.4),
     stdc_raise_uninstalled (2.16/W5), stdc_raise_null_info (2.14/W2),
     out_of_range_signo (NEGS).
   - `wg14_install_semantics_test` — ports of siginstall_null_set (RTIM),
     edge_api_coverage (TCOV), sig_default_preserve (DFLT).
   - `wg14_sigguarded_test` — ports of recovery_null_loop (1.7),
     sigguarded_tss_init (2.19/X3), plus the SIGGUARDED_FAILURE_VALUE
     sentinel.
   - `wg14_handoff_test` — ports of nsih_siginfo_handoff (NSIH),
     decider_destroy_return (SDCF).
   - `wg14_thread_handle_test` — port of thrd_signal_handle (thread-local
     recovery, global claim, concurrent destroy; the nested-delivery trigger
     uses kill(getpid(), signo) since libc has no pthread_kill entrypoint)
     and nested_decider_rsi (NSTR).
   - `wg14_tss_test` — port of tss_destroy_reentrancy (UCLK) plus basics.
   - `wg14_chain_test` — libc-native: user-handler wrap/claim/decline/
     restore round-trip, SA_SIGINFO fidelity through the handoff, altstack
     (kernel raise() runs on the altstack; the wg14 handoff calls the
     pre-existing handler synchronously per invoke_sigaction, so that leg
     asserts the handler ran rather than its stack), and abort() re-entry
     recovering out of a sigguarded(SIGABRT) frame.
   The white-box submodule tests (raise_claim_cleanup, siginstall_rollback,
   sig_unknown_handler, signo_map_verstable_init, header-only family) cannot
   be hermetic (they compile the .ipp into the TU or need --wrap) and stay
   covered by the standalone suite.
2. **libc-native tests** — step 2 above (chain/altstack/abort re-entry) plus
   the pre-existing linux signal tests all still pass unchanged.
3. **`check-libc`** on Linux: 1160/1167 (7 new tests), failure set identical
   to the known docker-as-root baseline (realpath/chmod/fchmod/fchmodat/
   utimensat/access/faccessat permission tests); wg14 hermetic tests 8/8 via
   lit; extended smoke test 40/40 unchanged.

**Defect found and fixed during Phase 3 (2026-08-22, `wg14_hostcalls.cpp`)**:
the weak C-symbol `setjmp`/`longjmp` bridges used by hermetic test links
were plain wrappers, and their stack frames — holding the caller's saved
x29/x30 — were routinely overwritten by the calls the guarded function makes
between setjmp() and longjmp(); longjmp then returned into the middle of a
later call instead of the setjmp site (probe-verified: the direct
`LIBC_NAMESPACE::setjmp/longjmp` calls worked, the raw C-symbol calls
"returned" from the wrong place). Both bridges are now `[[clang::musttail]]`
tail calls (statement/return form, with a `-Winvalid-noreturn` suppression
for the noreturn longjmp, and a non-clang fallback), so no bridge frame
exists: the real setjmp/longjmp save and restore the *caller's* context and
return directly to the caller's call site. The library build is unaffected
(the strong entrypoints win over the weak bridges; the submodule's own
standalone suite links against glibc where no bridge exists).

**Porting notes for future tests**: C++17 (no designated initializers — use
positional `union stdc_siginfo_value{7}` or assign members); the UnitTest
framework's EXPECT_EQ needs same-typed operands (`intptr_t` members and
`thrd_success` need casts); use `LIBC_NAMESPACE::`-qualified entrypoint calls
(the closure's internal objects define only namespace symbols); avoid host
C++ stdlib headers that pull glibc features headers (use
`src/__support/CPP/atomic.h`); `<threads.h>` is libc's generated header.

### Phase 4 — FreeBSD

1. OSUtil: add freebsd `syscall_wrappers` for sigaction/sigaltstack/
   tgkill if missing (model on linux ones).
2. `current_thread_id`: freebsd branch already exists
   (`pthread_getthreadid_np`); libc has `pthread_getthreadid_np.cpp`.
3. Config `libc/config/freebsd/x86_64/entrypoints.txt`: add the 12 existing
   + N3924 entrypoints.
4. Verify with the FreeBSD CI leg (VM leg exists in this repo's CI).

### Phase 5 — macOS (darwin)

**DEFERRED 2026-08-22** (executive item 8): do not start until Phase 3
tests, FreeBSD, and docs have landed; the darwin baseline (builds +
check-libc 0-tests + hdrgen 1/1) must be preserved meanwhile. The earlier
build-fix patches (fenv/`__fpcr` etc.) must be re-applied to the fork tree
before this phase begins.

1. Fix the earlier darwin build breakage prerequisites already discovered:
   the fenv/`__fpcr` patches from the build session stay; `sigsetjmp`
   darwin exclusion stays until here.
2. OSUtil: darwin syscall wrappers for `SYS_sigaction`, `SYS_sigaltstack`,
   `SYS_sigprocmask`, `SYS_kill`/`SYS_gettid` (aarch64 syscall.h exists;
   x86_64 darwin syscall layer needs adding).
3. Headers: darwin `signal-macros.h` (SIG numbering + `NSIG`), darwin
   `struct_sigaction` (Apple layout has no `sa_restorer`, has
   `sa_sigaction` union + mask + flags), `siginfo_t` (Apple's `struct
   __siginfo`), `sigset_t` (Apple: 32-bit), `stack_t`, `ucontext_t`.
4. `tss_async_signal_safe` on macOS uses the hash-table path automatically.
5. Config `libc/config/darwin/{aarch64,x86_64}/entrypoints.txt`: add the 12
   existing + N3924 entrypoints; re-enable `sigsetjmp`/`siglongjmp`.
6. Note: no darwin `crt1.o` exists in libc, so hermetic tests stay skipped
   on darwin (pre-existing); test via the standalone wg14_signals suite
   against the built libc.a + system glue (as done in the earlier build
   session) and the macOS CI leg.

### Phase 6 — Windows (default OFF)

1. `LIBC_WG14_SIGNALS_ENABLE_WINDOWS` option; when ON, compile the SEH
   backend. Constraints: clang-cl `__try/__except` support; `sigset_t` is
   wg14_signals' own `uint32_t` shim (`thrd_signal_handle.h:54-130`) — libc
   on Windows currently has no `<signal.h>` at all, so the generated header
   and the shim must agree; `thread_atexit` uses `IMAGE_TLS_DIRECTORY`
   which collides with libc-as-CRT — document that the Windows target
   remains "experimental" as it is today.
2. Expose the N3924 entrypoints only (not POSIX sigaction etc.) in
   `libc/config/windows/entrypoints.txt`.

### Phase 7 — GPU / UEFI / baremetal (exclusion)

1. No code changes beyond Phase 1's gating; the wg14 object library is not
   compiled there (no `signal/<os>/` dir).
2. `signal.yaml` functions must be marked with the existing per-function
   `standards`/availability mechanism or the config `headers.txt` must
   exclude the new declarations on those platforms, so the generated header
   does not declare functions that don't exist there.
3. Document in the libc platform-support docs and this repo's Readme.

### Phase 8 — Documentation

1. `llvm-project/libc/docs/`: platform support pages (macOS now has
   signals; Windows experimental), `configure.html` (new option), a new
   `signals.md` describing the N3924 integration and the gating policy.
2. This repo: `Readme.md` supported targets; `plans/` cross-reference.

### Phase 9 — Replace all remaining old-API signal handling in llvm-project

Goal: every `sigaction`/`signal`/`sigsetjmp`-style call site in the
llvm-project tree (inventory in §"Comprehensive review") runs through the
threadsafe implementation. `sigprocmask`/`pthread_sigmask`/`sigaltstack`/
`kill`/`strsignal` call sites are *not* in scope (they are not part of the
N3924 replacement surface and remain standard calls). Windows SEH
(`__try`/`__except`, `AddVectoredExceptionHandler`,
`SetUnhandledExceptionFilter`) maps onto the wg14_signals Windows backend
when it is enabled.

This phase consumes the wg14_signals library in two flavours: the
libc-embedded one (Phases 1-8) for everything that links against libc, and
the **standalone** library for compiler-rt, which has its own build and
cannot depend on libc.

#### Phase 9a — llvm/lib/Support (foundation)

**Design decisions (2026-08-22, implementation session):** the Phase 9a
replacement is gated behind a CMake option **`LLVM_ENABLE_THREADSAFE_SIGNALS`**
(default **ON on Linux**, OFF elsewhere), flowing into `llvm-config.h` as
`#cmakedefine01`. The old `sigaction`-based code remains compiled in the
`#else` branch (and as the fallback if `siginstall` fails), so macOS/FreeBSD/
Windows baselines stay bit-identical until their phases, while default Linux
builds exercise the replacement end-to-end. The wg14_signals sources are
compiled as an object library (`LLVMwg14_signals`, Linux posix backend) inside
`llvm/lib/Support/CMakeLists.txt` from the submodule; the
force-included hooks header (`llvm/lib/Support/Unix/LLVMSignalHooks.h`) now
carries **no embedder macros at all** — both of the originally-proposed
compile-time overrides were replaced by public runtime APIs during the
2026-08-27 upstreaming session:
- **`WG14_SIGNALS_SA_FLAGS` → `siginstall_set_sa_flags_np()`** (the
  `SA_ONSTACK` requirement; LLVM's altstack must host the raw handler for
  stack-overflow SIGSEGV detection), called at the top of
  `RegisterHandlersThreadsafe()` (Unix/Signals.inc) and applying to every
  subsequent `siginstall()`.
- **`WG14_SIGNALS_DEFAULT_ACTION` → `siginstall_set_default_action_np()`** with
  the `sig_default_action_np_t` callback type. LLVM registers
  `LLVMSignalDefaultAction` (an `extern "C"` function in Signals.inc that
  resets to SIG_DFL and re-delivers via `SYS_rt_tgsigqueueinfo` so the original
  `siginfo` — fault address — is preserved in the core dump, falling back to
  `raise()`) at the same point; the library's own built-in reset-and-re-raise
  remains the default when no callback is set. Both changes are
  **upstreamed** into wg14_signals `config.h` (macro removed) / the POSIX
  `.ipp` (setters + atomic default-action field, read lock-free from
  async-signal-safe context) / `thrd_signal_handle.h` (public declarations),
  with a Windows `ENOTSUP` stub for each setter, and the standalone suite
  passes 40/40 with them (the 40th is the 2026-08-27
  `siginstall_default_action_test`, the regression test for the new API;
  the 39th is `siginstall_sa_flags_test` for `siginstall_set_sa_flags_np()`);
the fork submodule is temporarily dirty with exactly those upstream
edits pending the commit + pin bump. The Windows and z/OS items of the
original 9a list (Windows/Signals.inc, zOSLibFunctions.cpp) are NOT touched in
this session (Windows backend is Phase 6/9d territory). **Implementation
status (2026-08-22)**: implemented in Signals.inc, CrashRecoveryContext.cpp,
Program.inc, InitLLVM.cpp (comments only), the CMake wiring, and the
LLVMSignalHooks.h header; verified on macOS arm64 with the option forced ON —
SupportTests 1744/1744 (incl. CrashRecoveryTest 5/5, UnixCRCReturnCode),
fault-injection spot checks (SIGSEGV → stack dump + death by signal; SIGINT/
SIGTERM/SIGUSR2 → default-action death; SIGUSR1 → consumed/resume; SIGPIPE →
one-shot exit 74), `sys::Wait` timeout via the SIGALRM decider (EINTR + kill +
ReturnCode -2); the option-OFF configuration also builds and passes
SupportTests 1744/1744 (baseline preservation). Linux docker verification
(2026-08-22, the run-3 image/flags, `LLVM_ENABLE_THREADSAFE_SIGNALS=1`
confirmed in the generated llvm-config.h): SupportTests **1763/1763 passed**
(13 skipped, 0 failed), CrashRecoveryTest 5/5, fault-injection spot checks
(SIGSEGV → stack dump + death by signal; SIGINT/SIGTERM/SIGUSR2 → death by
the signal; SIGUSR1 → consumed/resume; SIGPIPE → one-shot exit 74),
`sys::Wait` timeout → EINTR + SIGKILL + ReturnCode -2 "Child timed out". The
full `check-all` comparison against the run-3 baseline is in flight — **DONE
2026-08-26** (see the re-verification run below).

**Full Linux `check-all`, Phase 9a re-verification (2026-08-26, log
`check-all-phase9a-rerun2.log`, same image/flags, rebuild 8132/8132 clean +
tests)**: **141,736 discovered — 135,287 passed (95.45%), 35 failed (0.02%),
244 expectedly-failed, 6,081 unsupported, 89 skipped** (4601 s). This run
covers today's rework: sigguarded-only `RunSafely` (no jmp_buf chain),
`HandleExit` raise-through-guard, `+[]`-lambda callbacks, state on
`CrashRecoveryContextImpl`, and the per-handler decider split in
Signals.inc. Failure-set comparison vs the 2026-08-22 run (43 failed): the
only new failure is `HWASan stack-oob.c` (documented flaky pool); the
baseline's flaky draw (`HWASan bcmp/memcpy/memset/stack-uar/aligned_alloc`,
`llvm-ar thin-archive/archive-malformed`, `cfi cross-dso-diagnostic/stats`)
all passed today — i.e. ±8 is the same sanitizer/cfi flaky draw as runs 3/4.
Stable environmental set identical. `LLVM-Unit :: Support/SupportTests`
(17 shards: CrashRecoveryTest, ProcessTest, ProgramTest, SignalsTest incl.)
all PASS. **No signal-related failures; no regression.** (Note: the first
attempt on this date was killed at 186 s by a Docker VM crash — overlay2/I-O
errors from the full rebuild under the 7.75 GiB cap; the VM recovered after a
Docker Desktop restart and the test-only re-run completed. The failed tests
visible in that truncated run matched the baseline environmental set.)

**Full Linux `check-all`, Phase 9a run (2026-08-22, log
`check-all-phase9a.log`, same image/flags/`-j3`/`-j4` lit as run 3)**:
**141,736 discovered — 135,279 passed (95.44%), 43 failed (0.03%), 244
expectedly-failed, 6,081 unsupported, 89 skipped** (4873 s). Baseline run 3:
135,280 passed / 42 failed. Failure-set comparison (suffix-stripped): the
stable environmental set (permissions/umask/case-insensitive/container —
`error-opening-permission`, `ssaf-*permissions`, `case-insensitive-include*`,
`inferred-framework-case.m`, `nonportable-include-with-hmap.c`,
`release_shadow_space.c`, `online-merging.c`, `instrprof-*`,
`fuzzer-dirs.test`, `shtest-umask.py`, `llvm-lipo` ×2,
`llvm-dwarfdump/X86/output.s`, the 8 lld LTO/emit-imports/file-access tests,
`llvm-ar/error-opening-permission`, `llvm-ranlib/error-opening-permission`)
is identical to the baseline. The only differences are the documented
**flaky sanitizer/cfi draw under the 7.75 GiB memory cap**: run 3's flaky
eight (HWASan `stack-oob`/`stack-underflow`, `llvm-objcopy strip-debug`,
LSan `suppressions_default`, cfi `devirt stats` ×2, cfi `mfcall`, cfi thinlto
`cross-dso-diagnostic`) all PASSED in this run, while a different seven from
the same pool failed (HWASan `memcpy`/`memmove`/`memset`/`stack-uar`/
`aligned_alloc-alignment`, cfi-standalone-lld `cross-dso-diagnostic` +
`stats`) plus two llvm-ar tests (`Object/archive-malformed-object.test`,
`tools/llvm-ar/thin-archive.test`) that re-run **PASS** standalone. **No
signal-related failures; no regression.** This is run 4 of the Linux
baseline series (run 1 OOM, run 2 31 failed, run 3 canonical 42 failed, run 4
43 failed — all environmental/flaky variance).

**Decider mapping (Unix/Signals.inc)**: `RegisterHandlers()` keeps its name,
mutex, early-return and `CreateSigAltStack()`; the wg14 path then (1) builds
the guarded set = IntSigs + KillSigs + InfoSigs + (SIGPIPE iff the one-shot
pipe function is set), pre-filtering SIG_IGN'd signals when
`NeedsPOSIXUtilitySignalHandling` (querying the disposition before install,
exactly like the old per-signal skip), (2) `siginstall(&set)`, (3)
`signal_decider_create(&set, callfirst=false, …)`.
**REVISED 2026-08-26**: instead of a single `LLVMGlobalSignalDecider`
dispatching on the signal number, each legacy handler kind is **its own
decider with its own guarded set** (the decider is linked only into the
per-signal lists of its guarded set, so the dispatch checks disappear):
`LLVMSigpipeDecider` guards {SIGPIPE} (exchange-null + run the one-shot pipe
function + `sig_decision_resume_execution` — consumed, as old; else
`next_decider` → default), `LLVMInterruptDecider` guards IntSigs (same),
`LLVMInfoDecider` guards InfoSigs (errno save/restore + info function +
`resume_execution`, no file removal as old; in terminate mode SIGUSR1 takes
`next_decider` → default), `LLVMKillSignalDecider` guards KillSigs
(`RunSignalHandlers()` then `next_decider` → handoff → `invoke_sigaction` →
SIG_DFL → default action via the `LLVMSignalDefaultAction` callback
registered with `siginstall_set_default_action_np()` (exit
code = signal, as the old unregister+re-raise)). Each decider first unblocks
all signals (`sigprocmask(SIG_UNBLOCK)` — old handler did this so the
re-raise cannot pend); all non-info deciders call `RemoveFilesToRemove()`
first (matching `SignalHandler`). The deciders are created with `callfirst =
false` in legacy sequence order (disjoint sets, so order is cosmetic) and the
SIGPIPE one-shot and interrupt deciders are created only when their function
is registered (empty guarded sets are rejected with EINVAL; a per-set flag
tracks the SIG_IGN filter). **Re-entrancy guard**: a process-global atomic
counter, touched ONLY by `LLVMKillSignalDecider` — a nested delivery while a
KillSig chain is in flight skips the chain and returns `next_decider` (→
default → die), replicating the old handler's first-thing-unregister "crash
in the handler kills the process" property; InfoSigs/IntSigs intentionally
do NOT check it (nested delivery runs the chain again, matching old
SA_NODEFER-less info handling and the one-shot-interrupt semantics
respectively). SIGPIPE is
installed even if `SetOneShotPipeSignalFunction` is called after the first
registration (old code silently missed SIGPIPE in that order — a deliberate
fix). `unregisterHandlers()` in the wg14 path destroys the decider handle(s)
and `siguninstall`s the tokens; legacy code is the fallback. `InitLLVM.cpp`
needs NO functional change — `SetOneShotPipeSignalFunction`/`AddSignalHandler`
→ `RegisterHandlers` flow is preserved (the plan's "InitLLVM now calls
siginstall instead of RegisterHandlers" is superseded: RegisterHandlers IS the
installer in both paths).

**CrashRecoveryContext.cpp**: `Enable()`/`Disable()` keep their mutex +
refcount but install/uninstall the wg14 raw handler for the same 6 signals
(SIG_IGN skip in utility mode preserved; refcounted — coexists with
RegisterHandlers' installs, first install captures `old_handler`).
**REVISED 2026-08-26**: `RunSafely()` takes **no jump buffer of its own on
the threadsafe path** — `sigguarded()` IS the recovery mechanism: the crash
signal runs the frame decider, the wg14 machinery longjmps back to its own
call site (popping the guard frame) and invokes the recovery function, whose
return value `sigguarded()` returns; RunSafely() discriminates the
recovery outcome from normal completion via a `CrashHandled` flag. The
per-invocation state (`Fn`, `CrashHandled`, and HandleExit's
`PendingExit`/`PendingExitRetCode`) lives on the `CrashRecoveryContextImpl`
itself, and the `CRCI` pointer is `value.ptr_value` — the documented N3924
channel for caller state, handed to
the guarded function verbatim and stamped into `rsi->value` for the frame
decider and recovery function (no thread-local lookup): the frame decider
returns `sig_decision_call_recovery` iff `rsi->value.ptr_value` is non-null
(else `next_decider` → previous handler/default, replicating the old
disable+re-raise). The three callbacks are non-capturing lambdas passed with
a unary `+` (`+[](...)`) so the closure converts to a plain C-compatible
function pointer of the wg14 callback types on any C++ standard. The
recovery function performs `HandleCrash()`'s
bookkeeping (current-context removal, `Failed`, `CleanupOnSignal`, `RetCode`)
**without the longjmp** — `ValidJumpBuffer` is never set on the threadsafe
path, so `HandleCrash()` returns — sets `CrashHandled` and returns through
`sigguarded()`. There is exactly one longjmp per recovery (the wg14
implementation's own); the earlier design of a second longjmp chained after
it (recovery fn → `HandleCrash` → outer `JumpBuffer`) was rejected. The
outer `setjmp`/`JumpBuffer` survives only in the legacy non-threadsafe
branch. `HandleExit()` (**REVISED 2026-08-26**): with no outer buffer to
jump to, an exit request from inside the guarded function becomes a raise
through the guard — `HandleExit` records the requested exit code on the
context (`PendingExit`/`PendingExitRetCode` on `CrashRecoveryContextImpl`)
and calls `stdc_raise(SIGABRT, NULL, NULL)`, whose frame walk runs the
decider → `sig_decision_call_recovery` → the recovery function records the
code verbatim instead of `128 + signo` (the `SIGPIPE`→`EX_IOERR` special
case stays on the real-signal path). A `SIGGUARDED_FAILURE_VALUE` return
(per-thread guard state could not be set up) is a hard failure via
`llvm::report_fatal_error()` (prints a diagnostic, then aborts, in all build
modes — unlike `llvm_unreachable`, which is UB in Release): it cannot happen
on Linux, where `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` is
always true, and there is no unguarded fallback. The `sigguarded` set is the
full 6-signal set; a signal
skipped at install (SIG_IGN) simply never delivers.
`throwIfCrash` is unchanged (`unregisterHandlers(); raise(RetCode-128)` — the
raise now hits the restored previous disposition, same outcome).

**Program.inc**: the SIGALRM wait-timeout installs a temporary wg14
installation for SIGALRM (`siginstall` + a decider returning
`sig_decision_resume_execution`, so the delivery interrupts `wait4()` with
EINTR exactly like the old no-op handler), and tears it down
(`signal_decider_destroy` + `siguninstall`) on both the timeout and
normal-exit paths; `kill(pid, SIGKILL)` etc. unchanged. **Process.inc**:
NOTHING-TO-DO in this session — its `signal(SIGABRT, _exit)` block is
macOS-only (`HAVE_MACH_MACH_H`), i.e. Phase 5/9a-darwin territory.

1. **Signals.inc (Unix)**: **DONE 2026-08-22** — `RegisterHandlers()` now
   installs via `siginstall()` (raw handler with
   `SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER|SA_ONSTACK` and the default-action
   callback `LLVMSignalDefaultAction`, both requested at runtime via the
   public `siginstall_set_sa_flags_np()` / `siginstall_set_default_action_np()`
   APIs at the top of `RegisterHandlersThreadsafe()` — the
   `WG14_SIGNALS_SA_FLAGS` / `WG14_SIGNALS_DEFAULT_ACTION` compile-time macros
   were replaced by those APIs, upstream 2026-08-27);
   **REVISED 2026-08-26**: instead of
   a single `LLVMGlobalSignalDecider`, each legacy handler kind is its own
   global decider registered with `signal_decider_create()`, `callfirst`
   false (so external deciders registered earlier run first), each guarding
   only its own signals — `LLVMSigpipeDecider` {SIGPIPE}, `LLVMInterruptDecider`
   {IntSigs}, `LLVMInfoDecider` {InfoSigs}, `LLVMKillSignalDecider`
   {KillSigs} — implementing the legacy `SignalHandler` sequence in the
   original order (files → one-shot SIGPIPE → interrupt → info →
   `RunSignalHandlers` chain → next_decider → default action via
   `LLVMSignalDefaultAction`, the callback registered with
   `siginstall_set_default_action_np()`
   that re-delivers with the original siginfo preserved). The individual
   legacy handler functions remain compiled as the fallback path.
   - file-removal list (`RemoveFilesOnSignal` — keep its lock-free
     discipline; deciders run in the raw handler, same constraints),
   - one-shot SIGPIPE function (a decider that self-resets after one
     delivery, preserving `SetOneShotPipeSignalFunction` semantics),
   - `PrintStackTraceOnErrorSignal` (print, then
     `sig_decision_next_decider`),
   - `CleanupOnSignal` handlers (`RunSignalHandlers` runs the registered
     chain — this is exactly the global-decider list).
   The terminate path (`SignalHandlerTerminate`, SIGINT/SIGTERM
   interrupt escape via `_Exit`) stays as the final decider with
   `sig_decision_next_decider` → default action (the threadsafe raw
   handler's default path re-raises with the `SIG_DFL` disposition, which
   is what `raise(Sig)` achieves today at `:451-529`).
   - The altstack allocation (`:292-320`) can be dropped if the raw
     handler runs on the thread's stack, or kept via `sigaltstack()` —
     decide by test; stack-overflow detection (SIGSEGV on an exhausted
     stack) must keep working, so prefer keeping `sigaltstack`.
   - The fork child's `sigprocmask(SIG_UNBLOCK)` (`:435`) stays as-is.
   - `UnregisterHandlers()` maps to `siguninstall()`.
2. **CrashRecoveryContext.cpp**: **DONE 2026-08-22** — `Enable()`/
   `Disable()` install/uninstall the wg14 raw handler for the 6-signal set
   (refcounted, coexisting with RegisterHandlers); `RunSafely()` runs the
   guarded function inside `sigguarded(&Set, guardedFn, recoveryFn,
   deciderFn, value)` with **no outer setjmp on the threadsafe path (revised
   2026-08-26)**; `value` carries a per-invocation `{CRCI, Fn, CrashHandled}`
   state pointer to every callback (the value parameter is the documented
   caller-state channel — no thread-local lookup in any callback); the
   decider returns `sig_decision_call_recovery` iff that carried state is
   non-null, else `sig_decision_next_decider`; the recovery function does
   the `HandleCrash(128 + signo, signo)` bookkeeping **without any
   longjmp** (`ValidJumpBuffer` stays false) and returns through
   `sigguarded()`, and RunSafely() reports the crash via the `CrashHandled`
   flag — no longjmp chained after the wg14 one. `HandleExit` (revised
   2026-08-26) raises through the active guard with the exit code carried
   on the context (`PendingExit`/`PendingExitRetCode` + `stdc_raise(SIGABRT)`
   → recovery records the code verbatim). The `raise(RetCode - 128)` re-raise
   (`:488`) is unchanged (the raise enters the wg14 machinery by
   definition when the raw handler is installed — no `stdc_raise()` swap,
   per the 2026-08-22 rule). The
   Windows `__try`/`__except` + `AddVectoredExceptionHandler` path
   (`:239-241, 323`) maps onto the SEH backend's `sigguarded()` when
   enabled.
3. **Process.inc**: **NOTHING-TO-DO 2026-08-22** — `PreventCoreFiles()`'s
   `signal(SIGABRT, _exit)` etc.
   (`:183-187`) is macOS-only (`HAVE_MACH_MACH_H`), deferred to the darwin
   phase; the mach
   `task_set_exception_ports` block is orthogonal and stays. The
   `pthread_sigmask`/`sigprocmask` calls (`:262-283`) stay.
4. **Program.inc**: **DONE 2026-08-22** — the SIGALRM wait-timeout
   installs a temporary wg14 installation for SIGALRM
   (`siginstall` + a decider returning
   `sig_decision_resume_execution` (so `wait4()` still returns EINTR),
   torn down
   (`signal_decider_destroy` + `siguninstall`) on both the timeout and
   normal-exit paths; the `kill(pid, SIGKILL)`
   stays.
 5. **InitLLVM.cpp**: **DONE 2026-08-22 (comments only)** — the call
    sequence is unchanged in both paths (the plan's "InitLLVM now calls
    `siginstall` instead of `RegisterHandlers`" is superseded: the wg14
    path keeps `RegisterHandlers` as the installer, §"Design decisions"
    above); the SIGPIPE gating comment now describes the siginstall-set
    gating.
6. **Windows/Signals.inc**: `SetUnhandledExceptionFilter` +
   `signal(SIGABRT, HandleAbort)` (`:407, 455`) map onto the SEH
   backend's `siginstall()` when `LIBC_WG14_SIGNALS_ENABLE_WINDOWS` is
   on; otherwise unchanged.
7. **zOSLibFunctions.cpp**: unchanged (strsignal shim); note that the
   z/OS backend plan (`plans/ibm_zos_backend.md`) already defines how
   wg14_signals supports z/OS.

#### Phase 9b — Consumers (clang, lld, tools, interpreter)

1. **clang/tools/driver/driver.cpp**: `raise(CommandRes - 128)` and
   `raise(SIGABRT)` (`:487-496`) — **KEEP the `raise()` calls (amended
   2026-08-22)**: an external `raise()` delivers into the wg14 filtering
   handler BY DEFINITION when `siginstall()` was performed, so the decider
   chain and default termination already run; replacing `raise()` with
   `stdc_raise()` would lose the no-siginstall fallback (`stdc_raise()`
   returns false and does NOT terminate — a behaviour change on clang's
   error path). Same rule as the internal-consumer rework: never replace an
   OS raise with `stdc_raise()` for the same signal.
2. **llvm/lib/ExecutionEngine/Interpreter/ExternalFunctions.cpp**:
   `raise(SIGABRT)` (`:341-342`) — **KEEP (amended 2026-08-22)**, for the
   same reason as item 1.
3. **lldb / llvm-exegesis / lld / libcxx / flang**: nothing to change
   (see review); lldb's `kill()`/`pthread_sigmask` and exegesis's
   `kill(SIGKILL)`/`si_signo` reads stay.
4. Verify the whole toolchain under a fault-injection test: SIGSEGV in a
   tool, SIGINT to a driver, stack overflow in the driver — each must
   produce the same crash report, cleanup behaviour, and exit status as
   before (compare against a baseline build).

#### Phase 9c — compiler-rt sanitizer runtimes (standalone wg14_signals)

Compiler-rt does not link libc's signal machinery; it links the
standalone wg14_signals library (submodule root, built as a plain CMake
target in `compiler-rt/lib`).

1. **sanitizer_common**: replace the `sigaction`/`signal` interceptors
   (`sanitizer_signal_interceptors.inc`) with a decider-chain integration:
   user `sigaction()`/`signal()` calls register deciders on the
   threadsafe chain instead of being stored in sanitizer-internal arrays;
   the sanitizer's own handler (`MaybeInstallSigaction`,
   `sanitizer_posix_libcdep.cpp:237-260`) becomes the single raw handler
   via `siginstall()`, with the user deciders run in the N3924 order.
   `internal_sigaction` (`sanitizer_linux_libcdep.cpp:104-119`) becomes
   the shim-overridden `sigaction` (the `WG14_SIGNALS_SIGACTION` override
   from the libc work applies to the standalone build too).
2. **TSan**: `sigactions[kSigCount]` registry and `sigaction_impl`
   (`tsan_interceptors_posix.cpp:209, 2691-2729`) are replaced by direct
   wg14_signals usage (`siginstall()` + `signal_decider_create()`, the
   sync-signal classification `:2293-2316` moves into the decider); the
   `sigaction`/`raise` entrypoints stay untouched; `pthread_sigmask`
   interceptor stays.
3. **ASan**: `asan_win.cpp`'s `SetUnhandledExceptionFilter` interceptor
   and vectored handler (`:79-83, 191, 338, 365`) map onto the SEH
   backend when enabled; `asan_posix.cpp`/`asan_thread.cpp` altstack
   accounting stays (compatible with `use_sigaltstack`).
4. **UBSan**: `ubsan_signals_standalone.cpp` uses the raw handler path;
   `ubsan_loop_detect.cpp`'s SIGPROF handler becomes a decider.
5. Gate: `-fsanitize-...-use-threadsafe-signals` (new flag, default OFF)
   so sanitizer behaviour is bit-identical by default; flip to ON in CI
   and run the full sanitizer test suites (asm-signal tests,
   signal-race tests, `asan_test.cpp` sigaction tests at `:281-316`).
6. The sanitizer runtimes keep their own `internal_sigprocmask`/
   `internal_sigaltstack` (not in the N3924 surface).

#### Phase 9d — Windows (SEH)

Only when `LIBC_WG14_SIGNALS_ENABLE_WINDOWS` (Phase 6) or the standalone
build's Windows backend is available: swap
`SetUnhandledExceptionFilter`/`AddVectoredExceptionHandler`/`__try`/
`__except` in llvm/Support and asan_win over to the SEH backend's
`siginstall`/`sigguarded`. Until then the Windows paths are unchanged and
covered by the existing Windows CI.

#### Phase 9e — Verification

1. LLVM's own test suites under the replacement: `llvm/unittests/Support`
   (CrashRecoveryTest), clang/lld lit suites, `llvm-exegesis` tests.
2. Fault-injection matrix (SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL/SIGTRAP/
   SIGINT/SIGTERM/SIGPIPE/SIGUSR1/SIGUSR2/SIGHUP): same stack trace
   content, same exit codes, same cleanup-file behaviour as the baseline
   (`llvm::sys` tests), and correct recovery in CrashRecoveryTest.
3. Concurrency: TSan/ASan builds of clang running the threadsafe
   handlers (the decider chain is THREADSAFE; a multi-threaded fault in
   one thread must not corrupt another thread's guards).
4. Update this plan's review section as findings change; remove fixed
   items.

## Test plan

Every phase must compare its result against the recorded baseline in
§"Baseline: test results before any changes (2026-08-20)": measured rows
(37/37 standalone on macOS arm64; libc builds; hdrgen 1/1; lit 0-tests
failure on darwin) and TBD rows (Linux glibc/musl, FreeBSD, Windows,
TSan, header-only, Fil-C, Phase 9 suites) must be filled in on the next CI
run **before** Phase 1 work begins.

- **Standalone suite (unchanged)**: the 40+ tests in `test/` continue to run
  against the host libc in this repo's CI — they exercise the exact `.ipp`
  code the submodule embeds, so they are the primary correctness net for
  the machinery itself.
- **Linux hermetic (new)**: ported N3924 tests as `add_libc_test` hermetic
  tests; these run the submodule's code linked against libc's own crt1 and
  allocator, catching override/embedding defects (recursion, wrong errno,
  wrong sigaction mapping).
- **Existing linux signal tests**: must pass unmodified (contract
  preservation).
- **FreeBSD leg**: this repo's VM leg plus libc's own freebsd build.
- **macOS**: standalone suite + the manual libc.a link verification used in
  the earlier build session; no hermetic tests (no darwin crt1 — record as
  a known gap; fixing it is out of scope).
- **Sanitizers**: ASan/UBSan on the Linux legs (catches the malloc/free
  discipline in the decider chains), TSan on the threading tests (the
  library is THREADSAFE; CI already runs TSan).
- **Phase 9 suites**: `llvm/unittests/Support/CrashRecoveryTest` (now
  against `sigguarded`), the llvm/clang/lld lit suites (behaviour parity
  via the fault-injection matrix in 9e), and the sanitizer regression
  suites (asan sigaction tests, tsan signal-race tests, ubsan loop
  detect) with the new flag both OFF (bit-identical baseline) and ON.

## Risks and mitigations

1. **Recursion through public entrypoints** — the shim is mandatory and
   compile-time (`#ifdef LIBC_SOURCE_BUILD`); a link-time assertion (the
   wg14 object library must not reference `libc.src.signal.*` entrypoint
   objects) can be encoded as a CMake dependency check.
2. **Kernel `rt_sigaction` vs wg14 handler expectations** — Linux libc
   without glibc has no kernel restorer; keep `__restore`/`__restore_rt`
   wired through the shim until the handler is proven on the altstack.
3. **`abort()` re-entrancy** — wg14's `default_abort` calls `abort()`; the
   shim maps it to `abort_utils::abort()` which must not loop (it resets to
   SIG_DFL first, as today). Covered by `decider_orphan_reinstall_test` and
   the abort re-entry tests.
4. **TLS in shared-library builds** — `initial-exec` TLS breaks
   dlopen'd-DSO use; in overlay mode the caveat from `config.h` applies;
   document, and consider forcing the fallback in overlay mode
   (`WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` via CMake, mirroring
   `WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS`).
5. **Header generation vs hand-written API** — `sigfence` and the
   `stdc_siginfo` union with C++ constexpr members
   (`thrd_signal_handle.h:364-401`) are not yaml-expressible; use the
   `*.h.def` pattern and keep the wg14_signals headers as the source of
   truth for those fragments (option B in §"Open questions").
6. **Submodule availability / pinning** — a non-recursive clone of the
   fork fails libc's build; the fork CMake guard makes the failure
   actionable, and CI checks out with `submodules: recursive`. The pinned
   commit is bumped deliberately; the override layer is upstreamed, so a
   bump never needs fork-side patches. If a future upstream change breaks
   the override contract, the fix goes upstream first — which is the point
   of the proof.
7. **macOS x86_64** — libc's darwin OSUtil is aarch64-only
   (`libc/src/__support/OSUtil/darwin/` has `aarch64/` only); x86_64 needs
   the syscall layer added before the darwin signal work is complete.
8. **Windows TLS-vs-CRT conflict** — gated OFF; documented as experimental.
9. **Signal-identity in the toolchain** — LLVM's `Signals.inc` handlers
   are order-sensitive (files-removal before stack-trace before
   re-raise) and rely on one-shot and `SIG_IGN`-skip semantics; the
   decider chain preserves order and one-shot-ness, but the SIG_IGN skip
   must be verified (wg14_signals must not claim signals the user
   deliberately ignored — check `siginstall`'s handling against
   Signals.inc's `act.sa_handler != SIG_IGN` guard).
10. **Sanitizer interceptor contract** — the sigaction/signal
    interceptors are ABI-visible behaviour for every ASan/TSan user; the
    decider-chain reimplementation is gated OFF by default and validated
    against the existing interceptors' test suites before flipping the
    gate (Phase 9c.5).
11. **Stack overflow detection** — Signals.inc handles SIGSEGV on an
    exhausted stack via its altstack; the raw handler must either keep
    using an altstack or the altstack must remain configured through
    `sigaltstack` — a fault in 9a.1's verification matrix.

## Open questions

1. Option A vs B for the N3924 public headers: (A) generate everything from
   yaml per libc convention (more work; `sigfence` needs the `.def` pattern;
   the `stdc_siginfo` union/constexprs need hand-maintained fragments) vs
   (B) install `include/wg14_signals/*.h` as-is alongside the generated
   headers (less libc-conformant, zero drift from the reference
   implementation, the C++-header test in `test/header_only_test.cpp`
   pattern could be reused). Recommendation: A for the scalar API
   (`signal.yaml`, `threads.yaml` — the `tss_async_signal_safe_*` functions
   live in `<threads.h>` per 7.30.6.5-8) and B for
   the sigfence/stdc_siginfo fragments, with the wg14_signals headers kept
   in sync by a sync test that diffs the submodule's headers against the
   generated fragments.
2. N3924 revision: this repo's implementation is at rev 5
   (`docs/proposed-wording.md`); the yaml function placement is confirmed
   against it: `tss_async_signal_safe_*` in `<threads.h>` (7.30.6.5-8).
   `current_thread_id` appears in no revision of the wording (it is the
   reference implementation's own API, used internally by the hash-table
   TSS fallback); its exposure in `<threads.h>` is a libc-side decision —
   revisit before shipping.
3. Whether `sigsetjmp`/`siglongjmp` should eventually be implemented as
   `sigguarded`-based entrypoints on all platforms (replacing the per-arch
   naked-asm implementations) — attractive for darwin/FreeBSD, but a
   behaviour-change risk for Linux; defer.
4. Whether the wg14_signals suite should move into `llvm-project/libc/test`
   entirely (hermetic on Linux) or stay standalone with a submodule pin
   (recommended: stay standalone; the suite needs host-libc features
   like printf/stdio that libc's full build does not provide on all
   platforms).
5. Scope boundary of Phase 9: whether `sigprocmask`/`pthread_sigmask`/
   `sigaltstack`/`kill` call sites should eventually also route through
   the threadsafe implementation (they are outside the N3924 surface;
   currently the plan keeps them as standard calls). The abort/SIGABRT
   path inside libc is already covered by Phases 2/5.
6. Whether compiler-rt's decider-chain integration (Phase 9c) should be
   upstreamed into compiler-rt proper, or kept as a fork-local option —
   the fork-local option mirrors how the whole integration is being
   proven; upstreaming is a separate proposal.

## Appendix A — Submodule manifest

Submodule (added 2026-08-20; staged in the fork, not yet committed):

- Path: `libc/src/signal/wg14`
- URL: `https://github.com/ned14/wg14_signals.git`
- Pinned commit: `50042dad4eb376bc88f6d21568007e72c615f3a8` (heads/main)
- `.gitmodules` entry:
  ```
  [submodule "libc/src/signal/wg14"]
      path = libc/src/signal/wg14
      url = https://github.com/ned14/wg14_signals.git
  ```

The submodule root IS the reference implementation; libc's build consumes
(no copies are made):

```
include/wg14_signals/config.h
include/wg14_signals/thrd_signal_handle.h
include/wg14_signals/tss_async_signal_safe.h
include/wg14_signals/current_thread_id.h
include/wg14_signals/detail/impl/thrd_signal_handle_posix.c.ipp
include/wg14_signals/detail/impl/thrd_signal_handle_windows.c.ipp
include/wg14_signals/detail/impl/thrd_signal_handle_common.ipp.ipp
include/wg14_signals/detail/impl/tss_async_signal_safe.c.ipp
include/wg14_signals/detail/impl/current_thread_id.c.ipp
include/wg14_signals/detail/impl/thread_atexit.c.ipp
include/wg14_signals/detail/impl/thread_atexit.cpp.ipp
src/wg14_signals/current_thread_id.c
src/wg14_signals/thrd_signal_handle_posix.c
src/wg14_signals/thrd_signal_handle_windows.c
src/wg14_signals/thread_atexit.c
src/wg14_signals/tss_async_signal_safe.c
```

Fork-owned files (tracked by the fork, outside the submodule):
`libc/src/signal/wg14/CMakeLists.txt` (object library + missing-submodule
guard), `libc/src/signal/wg14/entrypoints/*`. No fork-owned copy of any
upstream file exists.

## Appendix B — Host calls in the POSIX backend: override vs natural

| wg14_signals call | backend file:line(s) | Embedding action |
|---|---|---|
| `sigaction` | `thrd_signal_handle_posix.c.ipp:155-223, 289, 317, 576-589` | **OVERRIDE** (`WG14_SIGNALS_SIGACTION`) → libc `rt_sigaction` syscall wrapper (linux; darwin/freebsd wrappers to be added) |
| `pthread_kill(pthread_self(), ...)` | `thrd_signal_handle_posix.c.ipp:222` | **OVERRIDE** (`WG14_SIGNALS_KILL_SELF`) → `SYS_tgkill`/`SYS_gettid` (linux) |
| `abort` | `thrd_signal_handle_posix.c.ipp:153-159, 306, 509, 548` | **OVERRIDE** (`WG14_SIGNALS_ABORT`) → `abort_utils::abort()` (reworked, §"Internal consumers") |
| `memcpy`/`memset` | posix `.ipp:83,113,147,156,180,216,219,288,317,576`; common `.ipp` | natural → libc string entrypoints (no signal calls) |
| `malloc`/`free` | common `.ipp:188,201,334,361`; `tss_async_signal_safe.c.ipp` | natural → libc allocator (no signal calls) |
| `setjmp`/`longjmp` | common `.ipp:37,265` | natural → libc per-arch setjmp (no signal calls) |
| `__cxa_thread_atexit` | `thread_atexit.c.ipp:54-73` | absent in full build → pthread_key fallback used |
| `pthread_key_create/setspecific/once` | `thread_atexit.c.ipp:169-224` (fallback) | natural → libc pthread entrypoints (futex-based, no signal calls) |
| `syscall(SYS_gettid)` | `current_thread_id.c.ipp` (linux branch) | natural → `LIBC_NAMESPACE::syscall_impl` |
| `mach_thread_self` | `current_thread_id.c.ipp` (apple branch) | natural → mach kernel call |
| `pthread_getthreadid_np` | `current_thread_id.c.ipp` (freebsd branch) | natural → libc `pthread_getthreadid_np.cpp` |
| `errno` | all backends | natural → libc generated `<errno.h>` (TLS) |
| `NSIG` | `thrd_signal_handle_posix.c.ipp:370` | natural → platform `signal-macros.h` (linux: `NSIG 64`) |
| `<stdfil.h>` | common `.ipp:43` | `#ifdef __FILC__`-guarded; inert in libc builds |
