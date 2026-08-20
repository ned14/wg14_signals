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
   existing standard C library.
3. Build-wire the wg14_signals library object in libc (compile the
   submodule's C sources directly under the full-build flags:
   `-ffreestanding -fno-builtin -nostdlibinc`, no `add_subdirectory` of the
   submodule) and run the existing wg14_signals unit-test suite against it
   on Linux first (§"Phase 1").
4. Extend the generated public headers (§"Public API surface"): extend
   `libc/include/signal.yaml` with the N3924 functions/types/macros and add
   `libc/include/tss_async_signal_safe.yaml` (or a hand-written header per the
   `*.h.def` pattern for the `sigfence` macro, which cannot be yaml-expressed).
5. Add the new entrypoint objects (`libc/src/signal/wg14/entrypoints/*.cpp`
   thin `LLVM_LIBC_FUNCTION` wrappers) and register them in
   `libc/config/linux/{x86_64,aarch64,arm,riscv}/entrypoints.txt`.
6. Re-point the existing Linux signal entrypoints (§"Replacing the existing
   entrypoints"): `libc/src/signal/linux/sigaction.cpp`, `signal.cpp`,
   `raise.cpp`, `sigprocmask.cpp`, `pthread_sigmask.cpp`, `sigaltstack.cpp`,
   `sigaddset.cpp`, `sigdelset.cpp`, `sigemptyset.cpp`, `sigfillset.cpp`,
   `kill.cpp` become thin wrappers over the wg14_signals machinery; the
   kernel-facing handler in `signal_utils.h` is retired in favour of the
   wg14_signals raw handler, with user handlers dispatched as deciders.
7. Re-point the internal consumers (§"Internal consumers"): `abort()` in
   `libc/src/stdlib/linux/abort_utils.h` (SIGABRT via the new raise path,
   SIG_DFL re-raise, unblock), `system()` in
   `libc/src/stdlib/linux/system.cpp`, `posix_spawn()` in
   `libc/src/spawn/linux/posix_spawn.cpp`, and `raise` usage anywhere else
   that calls `linux_syscalls::raise` / `rt_sigaction` directly.
8. Bring up macOS (darwin) (§"Phase 6"): add darwin OSUtil syscall wrappers
   (sigaction, sigaltstack, sigprocmask, pthread_kill/pthread_self via the
   existing darwin syscall layer), darwin `signal-macros.h`, generated
   `struct_sigaction`/`siginfo_t`/`ucontext_t` types, and the config
   entrypoints in `libc/config/darwin/{aarch64,x86_64}/entrypoints.txt`;
   macOS uses the fallback hash-table TLS (config.h already defaults
   `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0` on `__APPLE__`).
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
    entrypoints.
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
   `-cc1` invocation failed (`:491-496`). These rely on LLVM's crash
   machinery (installed by `InitLLVM`) running first — with the
   threadsafe implementation they become `stdc_raise()`.
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
| macOS arm64, clang, Debug C11 (local, no sanitizer toolchain) | **37/37 passed**, benchmarks 2/2 passed (5.34 s suite, 13.44 s benchmarks) |
| Linux glibc (clang/gcc × Debug/Release × C11/C23 × shared OFF/ON × fallback OFF/ON, ASan/UBSan) | TBD — last CI run on `main` was green; re-run `ctest -E benchmark` on the next CI push before Phase 1 work and record counts here |
| Linux musl (Alpine, C11/C23 × shared OFF/ON) | TBD — same |
| macOS CI legs (Debug/Release × C11/C23 × shared × fallback, sanitize toolchain) | TBD — same |
| Windows VS2022 (Debug/Release × C11/C17 × shared, ASan) | TBD — same |
| FreeBSD 15 VM (C11/C23) | TBD — same; note `header_only_build_test` is already excluded there (`-E "benchmark\|header_only_build_test"`) |
| TSan (ubuntu gcc/clang, macOS clang; C11/C23) | TBD — same; `TSAN_OPTIONS=... report_signal_unsafe=0` etc. per ci.yml |
| Header-only (ubuntu/macOS/windows × C11/C23) | TBD — same |
| Fil-C C11 (ubuntu x86_64) | TBD — same; `thrd_signal_sigfpe_handle_test`, `recovery_null_loop_test`, `header_only_build_test` excluded there |

The override-macro probe (Phase 1 step 2) will add a ninth configuration:
same suite with `WG14_SIGNALS_SIGACTION/ABORT/KILL_SELF` overridden —
that configuration must also pass 37/37.

### B. llvm-project libc (the fork, full runtimes build)

Configured and built in the earlier session (`cmake -G Ninja -S runtimes
-B build -DLLVM_ENABLE_RUNTIMES=libc -DLLVM_LIBC_FULL_BUILD=ON
-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++ -DPython3_EXECUTABLE=.../.venv/bin/python3.14`).

| Suite | Result (2026-08-20, macOS arm64) |
|---|---|
| `ninja -C build libc libm` | **builds** (`libc.a`, `libm.a` produced) |
| `ninja -C build check-libc` | **hdrgen_integration_test 1/1 passed; lit: 0 tests discovered** → target FAILS code 2. Known pre-existing darwin failure: full-build tests are hermetic and all skip on darwin because `libc.startup.darwin.crt1` does not exist. This is the baseline to preserve: the lit "no tests" failure must not get worse, and Phase 5 (darwin) may legitimately fix it. |
| Linux (glibc) check-libc | TBD — run on a Linux runner before Phase 1 (needs `-DLLVM_ENABLE_RUNTIMES=libc` with the same flags; expect the full hermetic suite to run there) |
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
  **discovery: the fork `ned14/llvm-project` has zero GitHub Actions runs
  (`total_count: 0`; `has_actions: false`) and upstream llvm-project's CI
  does not cover the fork's head (`ee1bf608d998` is fork-specific), so no
  CI baseline exists for the fork.** The Linux/Windows/FreeBSD legs MUST
  be baselined before Phase 1 work: either enable Actions on the fork and
  push the current head (recording the `check-all` matrix for linux/
  windows/freebsd), or run the same configure+`check-all` command on a
  Linux runner / the freebsd VM leg / a Windows runner. Record the
  results in the table below and mark each TBD row done. Without this,
  Phase 1-9 changes cannot be attributed to regressions on those
  platforms.
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
    became `${{ github.workspace }}`. The remaining prerequisite is a
    one-time repo-settings change (enable Actions on `ned14/llvm-project`),
    then push the current head to record the baseline rows.

Local run results (macOS arm64, Release, clang) — `check-all` **completed
2026-08-20** (single lit invocation, 1533.66 s):

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
| Fault-injection matrix (SIGSEGV/SIGABRT/SIGFPE/SIGBUS/SIGILL/SIGTRAP/SIGINT/SIGTERM/SIGPIPE/SIGUSR1/SIGUSR2/SIGHUP into a tool: stack trace content, exit codes, cleanup-file behaviour) | TBD | gates 9a-9e |

Out-of-scope suites to baseline from CI once the fork's Actions is
enabled (or from a runner): `check-lldb`, `check-flang`, `check-libcxx`,
`check-mlir`, `check-openmp`, `check-polly`, `check-clang-tools`, plus the
same matrix on Linux/Windows/FreeBSD.

Every phase's verification step compares against the rows above; a row
changing from pass to fail (or a TBD row first failing) is a regression
to resolve before proceeding. Known failures today: §B's darwin lit
0-tests failure and the standalone suite's platform exclusions (§A).

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
| `sigaction` | the libc `sigaction` entrypoint will BE the registry → recursion | `rt_sigaction` syscall wrapper (`libc/src/__support/OSUtil/linux/syscall_wrappers/rt_sigaction.h`); darwin/freebsd wrappers to be added |
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
- **`libc/include/tss_async_signal_safe.yaml`** (new header): the four
  `tss_async_signal_safe_*` functions plus the opaque handle type and
  `tss_async_signal_safe_attr`.
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

The existing Linux entrypoints keep their names and contracts but change
implementation:

- **`sigaction()` / `signal()`**: the wg14_signals raw handler
  (`raw_signal_handler`, installed by `siginstall()` with
  `SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER`, `thrd_signal_handle_posix.c.ipp:576-589`)
  becomes the single kernel-level handler for every installed signal.
  `sigaction()`/`signal()` become registry operations: they store the user
  handler (function pointer + `sigaction` struct, incl. `SA_SIGINFO`,
  `sa_mask`, `sa_flags`, `sa_restorer`) in the per-signal decider chain
  (`signal_decider_create`), and return the previously registered handler.
  `SIG_DFL`/`SIG_IGN` are represented as the existing default/ignore
  deciders. The `__restore` object (`libc/src/signal/linux/__restore.cpp`)
  can be retired once `raw_signal_handler` uses the kernel restorer
  (verify: the wg14 raw handler currently relies on the kernel-provided
  restorer `SA_RESTORER` behaviour on glibc; on Linux libc the
  `rt_sigaction` syscall requires an explicit `sa_restorer` — the shim must
  keep `__restore` or use `SA_RESTORER` with the same `__restore_rt` stub).
- **`raise()`**: becomes `stdc_raise()` semantics — run the decider chain
  in-process for the current thread, hand off to the previously installed
  handler (which is now the decider chain) if nothing claims it, and fall
  back to the kernel default for `SIG_DFL`. This is a behavioural
  improvement (user handlers run with full `siginfo_t`), and it is exactly
  what `abort()` needs.
- **`sigprocmask()` / `pthread_sigmask()`**: thin wrappers over
  `rt_sigprocmask` as today (no change in machinery; the wg14 code does not
  call these, so no recursion concern — but see the abort unblock path).
- **`sigaltstack()`**: unchanged syscall wrapper (the wg14 raw handler runs
  on the altstack if the user sets one; no conflict).
- **`kill()`**: unchanged (a different process's raise; wg14_signals does not
  intercept it).
- **sigset helpers** (`sigaddset`/`sigdelset`/`sigemptyset`/`sigfillset`):
  unchanged implementation; they remain plain bit operations on libc's
  `sigset_t` (note: wg14_signals' own helpers on Windows are a different
  `uint32_t` type — not relevant on POSIX targets, where libc's `sigset_t`
  and the POSIX semantics in `thrd_signal_handle.h:59-111` comments apply).

### Internal consumers

- **`abort()`** (`libc/src/stdlib/linux/abort_utils.h`): rework to raise
  SIGABRT through the new raise path (which consults deciders, i.e. user
  handlers, matching C11 `abort`'s requirement that a returning handler is
  followed by default action), then reset to `SIG_DFL` via the shim's
  raw sigaction, re-raise, unblock, `exit(127)` — same sequence as today but
  through the shim so the wg14 code's `abort()` calls (POSIX `.ipp:153-159,
  306, 509, 548`) resolve to this internal one.
- **`system()`** (`libc/src/stdlib/linux/system.cpp`) and **`posix_spawn()`**
  (`libc/src/spawn/linux/posix_spawn.cpp`): they use `signal()`/`sigaction`
  bookkeeping for SIGINT/SIGQUIT/SIGCHLD around the child; re-point to the
  new registry (functionally identical, now routed through the decider
  registry).
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
2. Upstream (this repo): add the override macros to `config.h`
   (`WG14_SIGNALS_SIGACTION`, `WG14_SIGNALS_ABORT`,
   `WG14_SIGNALS_KILL_SELF`) defaulting to the standard calls, and route
   the three call-site families in the POSIX `.ipp` through them. Prove
   the interface: add a CMake probe (or CI leg) in this repo that builds
   the library with all three macros overridden to simple wrappers and
   runs the full test suite — in both configurations.
3. Add the fork-owned `libc/src/signal/wg14/CMakeLists.txt` object
   library (no `add_subdirectory` of the submodule) with the
   missing-submodule FATAL_ERROR guard; wire into
   `libc/src/signal/CMakeLists.txt`.
4. Compile under full-build flags; fix freestanding issues surfaced by the
   build (verify `<stdatomic.h>` availability from clang, the pthread_key
   fallback resolves to libc's generated `<pthread.h>`).
5. Gate: only compiled when the platform has the prerequisites
   (`LIBC_TARGET_OS` in linux/darwin/freebsd) — the existing pattern of
   `if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${LIBC_TARGET_OS})` already
   isolates platforms.
6. Smoke test: build `ninja -C build libc` and run the submodule's own
   suite against the built `libc.a` on Linux (Linux has `crt1.o`).

### Phase 2 — Linux entrypoints + config

1. Add wrapper entrypoints under `libc/src/signal/wg14/entrypoints/`.
2. Extend `signal.yaml` + `llvm-libc-types/` + `hdr/` proxies.
3. Add the entrypoints to `libc/config/linux/{x86_64,aarch64,arm,riscv}/
   entrypoints.txt`.
4. Add the new syscall wrappers (`sigaltstack`, `gettid`/`tgkill`) to
   `libc/src/__support/OSUtil/linux/syscall_wrappers/`.
5. Re-point the 12 existing linux signal entrypoints per §"Replacing the
   existing entrypoints"; delete `signal_utils.h`'s kernel-handler logic and
   `__restore.cpp` only after proving the wg14 raw handler path.
6. Rework `abort_utils.h`, `system.cpp`, `posix_spawn.cpp` per §"Internal
   consumers".

### Phase 3 — Linux tests

1. Port the N3924-relevant tests from `test/` (`thrd_sigfpe_test.c`,
   `thrd_signal_handle_test.c`, `sigguarded_nonsignal_exception_test.c`,
   `nested_decider_rsi_test.c`, `raise_claim_cleanup_test.c`,
   `siginstall_*`, `decider_*`, `tss_*`, `nsih_siginfo_handoff_test.c`,
   `stdc_raise_*`, `edge_api_coverage_test.c`) as hermetic libc tests under
   `libc/test/src/signal/wg14/` (C11, `add_libc_test` in full-build mode).
2. Add libc-native tests: existing linux signal tests must still pass
   unchanged (they test the public contracts); add tests for handler
   chaining (user `sigaction` then `siginstall` decider then default),
   `SA_SIGINFO` fidelity, altstack, and `abort()` re-entry.
3. Run `ninja -C build check-libc` on Linux.

### Phase 4 — FreeBSD

1. OSUtil: add freebsd `syscall_wrappers` for sigaction/sigaltstack/
   tgkill if missing (model on linux ones).
2. `current_thread_id`: freebsd branch already exists
   (`pthread_getthreadid_np`); libc has `pthread_getthreadid_np.cpp`.
3. Config `libc/config/freebsd/x86_64/entrypoints.txt`: add the 12 existing
   + N3924 entrypoints.
4. Verify with the FreeBSD CI leg (VM leg exists in this repo's CI).

### Phase 5 — macOS (darwin)

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

1. **Signals.inc (Unix)**: replace `RegisterHandlers()`'s raw
   `sigaction` calls with `siginstall()` (which installs the threadsafe
   raw handler with `SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER`). Each existing
   LLVM handler becomes a global decider registered per signal with
   `signal_decider_create()`, in the current relative order:
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
2. **CrashRecoveryContext.cpp**: replace `RunSafely`'s handler
   install/uninstall + `sigsetjmp`/`siglongjmp` with
   `sigguarded(&guarded, RunSafelyOnThread, ...)` over
   `{SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP}`, with a decider
   returning `sig_decision_call_recovery` for the crash-recovery path and
   `sig_decision_next_decider` when recovery is not wanted. The
   `raise(RetCode - 128)` re-raise (`:488`) becomes `stdc_raise()`. The
   Windows `__try`/`__except` + `AddVectoredExceptionHandler` path
   (`:239-241, 323`) maps onto the SEH backend's `sigguarded()` when
   enabled.
3. **Process.inc**: `PreventCoreFiles()`'s `signal(SIGABRT, _exit)` etc.
   (`:183-187`) becomes a set of global deciders that call `_exit` (or a
   `sig_decision_*` that terminates); the mach
   `task_set_exception_ports` block is orthogonal and stays. The
   `pthread_sigmask`/`sigprocmask` calls (`:262-283`) stay.
4. **Program.inc**: the SIGALRM wait-timeout handler (`:385-455`)
   becomes a temporary global decider on SIGALRM returning
   `sig_decision_resume_execution` (so `wait4()` still returns EINTR),
   installed around `sys::Wait` and removed after — or a `sigguarded()`
   region with an alarm-raising `stdc_raise`; the `kill(pid, SIGKILL)`
   stays.
5. **InitLLVM.cpp**: the SIGPIPE `sigaction` gating (`:95-100`) moves
   into the one-shot SIGPIPE decider from 9a.1; `InitLLVM` now calls
   `siginstall` instead of `RegisterHandlers`.
6. **Windows/Signals.inc**: `SetUnhandledExceptionFilter` +
   `signal(SIGABRT, HandleAbort)` (`:407, 455`) map onto the SEH
   backend's `siginstall()` when `LIBC_WG14_SIGNALS_ENABLE_WINDOWS` is
   on; otherwise unchanged.
7. **zOSLibFunctions.cpp**: unchanged (strsignal shim); note that the
   z/OS backend plan (`plans/ibm_zos_backend.md`) already defines how
   wg14_signals supports z/OS.

#### Phase 9b — Consumers (clang, lld, tools, interpreter)

1. **clang/tools/driver/driver.cpp**: `raise(CommandRes - 128)` and
   `raise(SIGABRT)` (`:487-496`) → `stdc_raise()` (identical observable
   behaviour: deciders run, then default disposition terminates).
2. **llvm/lib/ExecutionEngine/Interpreter/ExternalFunctions.cpp**:
   `raise(SIGABRT)` (`:341-342`) → `stdc_raise(SIGABRT, NULL, NULL)`.
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
   (`tsan_interceptors_posix.cpp:209, 2691-2729`) are subsumed by the
   decider chain; the sync-signal classification (`:2293-2316`) moves
   into the decider; `pthread_sigmask` interceptor stays.
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
   (`signal.yaml`, `tss_async_signal_safe.yaml`, `threads.yaml`) and B for
   the sigfence/stdc_siginfo fragments, with the wg14_signals headers kept
   in sync by a sync test that diffs the submodule's headers against the
   generated fragments.
2. N3924 revision: this repo's implementation is at rev 5
   (`docs/proposed-wording.md`); the yaml function placement
   (`current_thread_id` in `<threads.h>`) should be confirmed against it.
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
