# Add a `thrd_signal_handle` backend for IBM z/OS (AMODE 64)

## Goal

Add a new platform backend, `include/wg14_signals/detail/impl/thrd_signal_handle_zos.c.ipp`
(+ `src/wg14_signals/thrd_signal_handle_zos.c`), implementing the full public API
(`sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`,
`sigfillset_asynchronous_debug`, `sigguarded`, `stdc_raise`, `siginstall`,
`siguninstall`, `siguninstall_system`, `signal_decider_create`,
`signal_decider_destroy`) on IBM z/OS, in AMODE 64, running under z/OS UNIX
System Services with the `POSIX(ON)` Language Environment runtime option.

We have no access to a z/OS system, so everything below is derived from close
study of the IBM documentation (z/OS 3.1.0 Language Environment AMODE 64
exception-handler topics — the `a6eh-*` pages the user pointed at — the z/OS
C/C++ Runtime Library Reference, the LE Programming Guide for 64-bit, the XL
C/C++ User's Guide and Language Reference) and from real-world z/OS code in
the open source Eclipse OMR (J9 JVM) z/OS port and the Go z/OS port, which
exercise exactly the APIs this plan proposes. Where the documentation is
ambiguous or self-contradictory, the plan says so and prescribes a
machine-side probe (Appendix A) to resolve it.

## Ordered implementation task list (for the implementing agent)

Paths line-referenced against the current tree.

1. `config.h:56` — add `&& !defined(__MVS__)` to the
   `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` default (z/OS always uses the
   tss_async_signal_safe fallback).
2. `current_thread_id.c.ipp:86-89` — add the `#elif defined(__MVS__)`
   branch (`pthread_self()`); `#include <pthread.h>` under `__MVS__`.
3. `thrd_signal_handle_common.ipp.ipp` — two z/OS changes:
   a. `struct sig_global_state_tss_state_per_frame_t` (line 265): the frame
      buffer must be `sigjmp_buf` on z/OS, not `jmp_buf` — `sigsetjmp()` /
      `siglongjmp()` take a `sigjmp_buf`, which on z/OS is a distinct,
      *larger* type than `jmp_buf` (it additionally carries the signal
      mask), so passing the `jmp_buf` field would be a compile error at
      best and a buffer overflow at worst. Guard the member:
      ```c
      #ifdef __MVS__
      sigjmp_buf buf;  // z/OS: sigsetjmp()/siglongjmp() also restore the signal mask
      #else
      jmp_buf buf;
      #endif
      ```
      (`sigjmp_buf` is visible because the z/OS .ipp defines `_POSIX_SOURCE`
      before any include; the common .ipp's own `<setjmp.h>` include at line
      37 then declares it. Windows and POSIX backends are untouched.)
   b. add the four z/OS uncatchable signals to the 6 signal loops (see
      §"Skip the uncatchable z/OS signals").
4. `thrd_signal_handle.h` — `__MVS__` branch for `stdc_siginfo_siginfo_t`
   (alias `siginfo_t`) and the header-only include selection
   (lines 359-373 and 641-647).
5. New `include/wg14_signals/detail/impl/thrd_signal_handle_zos.c.ipp`
   (Phase 1, signals-only) per §"Implementation"; wrapper
   `src/wg14_signals/thrd_signal_handle_zos.c`; wire into `CMakeLists.txt`
   (add the `WG14_SIGNALS_ZOS` detection near `project()`, then use it for
   the backend source at lines 85-91, the warning-flag branch at lines
   188-191, and the FTM bypass at lines 129-164).
6. `test/CMakeLists.txt` — z/OS exclusions (see §"Test plan"; model the
   `siginstall_rollback_test` guard at `test/CMakeLists.txt:24`).
7. Documentation updates per §"Documentation updates".
8. On a real z/OS machine: run Appendix A probe, then full test suite.

## Research: documentation studied

All URLs are the IBM Docs topic ids (`?topic=`), retrievable at
`https://www.ibm.com/docs/api/v1/content/en/zos/3.1.0?topic=<id>`:

| Topic id | What it establishes |
|---|---|
| `a6eh-scope-nesting-exception-handlers` (user-provided) | AMODE 64 exception handlers registered by `__set_exception_handler()` are **stack-frame scoped**: active for the registering frame and everything it calls, automatically de-registered when the registering function returns, and when a longjmp-type call jumps back past the registering frame. Nested registration is LIFO; only the most recently registered handler is driven for a condition. |
| `a6eh-handling-exceptions` | The handler receives a `struct __cib *` (Condition Information Block); the only valid exits are longjmp-type calls, `exit()`/`_exit()`, `pthread_exit()`, `__cabend()`/`abort()`. **It must never return** (returning ⇒ `pthread_exit(-1)` with `POSIX(ON)`, `exit(-1)` otherwise). While the handler runs it is suspended; exceptions during the handler are processed as if no handler existed. |
| `functions-set-exception-handler-register-exception-handler-routine` | `__set_exception_handler(void(*)(struct __cib *, void *), void *user_data)` — AMODE 64 only; returns 0/-1 (EINVAL). Only invoked for the condition→signal families CEE341–CEE358 + CEE35I (program checks and retryable abend), **bypassing signal generation entirely** when a handler is registered. Requires `TRAP(ON)` or `TRAP(ON,NOSPIE)`. Blocks/ignores of SIGABND/SIGFPE/SIGILL/SIGSEGV have no effect on exception handlers. Full condition→signal mapping table captured in Appendix B. |
| `functions-reset-exception-handler-unregister-exception-handler-routine` | `int __reset_exception_handler(void)` — unregisters the handler registered **for the current stack frame**; returns 0, or -1/EINVAL if no handler is registered in the current frame. Consequence (confirmed): **called from inside a running exception handler it targets the handler's own frame and fails with EINVAL** — a running handler cannot unregister the `sigguarded()` frame that registered it. This settles Phase 2's fallback design (risk 3). |
| `signals-amode-64-exception-handlers` | When no exception handler is registered, program checks and ABENDs cause POSIX/ISO-C signals (SIGABND, SIGFPE, SIGILL, SIGSEGV). When one is registered, those signals are suppressed; all other (asynchronous) signals process normally. |
| `signals-signal-handlers` (+ children in the XL C/C++ Programming Guide) | Two signal models: ISO C (`signal()`/`raise()`, POSIX(OFF)) and POSIX.1 (`sigaction()`, `POSIX(ON)`). |
| `handlers-handling-exceptions` (LE 64-bit Programming Guide chapter 12–13, PDF `ceeam00_v3r1.pdf`) | Full LE condition-handling model: enablement step (TRAP option, SIG_IGN, masked exceptions), condition step (per-frame signal()-style handlers first, sigaction() actions only after all frames are visited), termination step. C condition tables (Table 15: C conditions & defaults; Table 16: S/370 interrupt codes 01–15 → SIGILL/SIGSEGV/SIGFPE; Table 17: abends → SIGABND) — captured in Appendix B. **"The signal mask is ignored for a signal caused by a program check."** Synchronous (incurring-thread) signals enter LE condition handling; asynchronous signals follow pure POSIX semantics. |
| Runtime Library Reference (PDF `bpxbd00_v3r1.pdf`): `sigaction()` | POSIX(ON) only. `struct sigaction { void (*sa_handler)(int); sigset_t sa_mask; int sa_flags; void (*sa_sigaction)(int, siginfo_t *, void *); }`. `SA_SIGINFO` supported; siginfo_t members **si_signo, si_errno (unused on z/OS), si_code, si_pid, si_uid, si_value — no `si_addr`**. Third arg points at `ucontext_t`. Full z/OS UNIX signal list (Table 57, Appendix C). **"If a signal catcher for a SIGABND, SIGFPE, SIGILL or SIGSEGV signal runs as a result of a program check or an ABEND, and the signal catcher executes a RETURN statement, the process will be terminated."** |
| Runtime Library Reference: `signal()` | ISO C model; supported signals in POSIX(OFF) (Table 59): SIGABND, SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM, SIGUSR1, SIGUSR2, SIGIOERR. Defaults for SIGUSR1/SIGUSR2/SIGINT/SIGTERM changed to abnormal termination at LE Release 3. Same RETURN-terminates rule quoted under POSIX(ON). |
| Runtime Library Reference: `__le_cib_get()`, `__cabend()` | `struct cib *__le_cib_get(void)` (from `<__le_api.h>`): returns the CIB for the current condition; valid in an LE exception handler, a POSIX(OFF) catcher, and a POSIX(ON) catcher **when the signal was generated and caught immediately to the same thread**; NULL + `EMVSERR` otherwise. `__cabend(int abendcode, int reasoncode, int clean_up)` (from `<ctest.h>`, AMODE 64): abnormal termination. |
| Runtime Library Reference: `siglongjmp()`/`sigsetjmp()` | Standard POSIX pair; restores signal mask if `sigsetjmp(env, savemask)` saved it; explicitly listed as a valid exit from LE exception handlers. |
| Runtime Library Reference: `pthread_kill()` | Standard; needs `_OPEN_THREADS` or `_UNIX03_THREADS`. |
| Eclipse OMR `port/unix/omrsignal.c`, `port/zos390/omrsignal_context.c`, `omrceeocb.h` | Production z/OS 64-bit signal code (J9 JVM): installs sigaction with `SA_SIGINFO|SA_NODEFER|SA_RESTART` and `sigemptyset(&sa_mask)`; dispatches per-thread handler chains; uses `siglongjmp()` to recovery buffers; on z/OS uses `__le_cib_get()` inside the catcher, reads `cib_abf`/`cib_pcf` to detect a real program check/abend, and reads the faulting address as `((__mch_t *)cib->cib_machine)->__mch_bea` (Breaking Event Address); for unhandled conditions it calls `__cabend()` (after `sigrelse(SIGABND)`). Checks `TRAP(ON,SPIE)` via the CEEOCB (`ceeocb()->ceeocb_opt[__trap]`, `ceeocb_trap_spie`) to decide whether resuming from a signal handler is supported. This is the closest real-world analogue of what this plan proposes; its patterns are adopted directly. |
| Go z/OS port (`golang.org/x/sys/unix/zsysnum_zos_s390x.go`, `zerrors_zos_s390x.go`) | `__set_exception_handler` = syscall 2274, `__reset_exception_handler` = 2275, `__le_cib_get` = 2110. Real z/OS signal numbering (Appendix C). `Siginfo` = {si_signo, si_errno, si_code, si_pid, si_uid, pad} — confirming **no si_addr** and the exact field order. |
| `zopencommunity/zoslib` `include/signal.h` | IBM-maintained wrapper: `#define NSIG 42`. |

## Platform facts and constraints that drive the design

1. **AMODE 64 only.** `__set_exception_handler()` is AMODE 64 only; the 31-bit
   model (CEEHDLR callable services, USRHDLR, `CEE3CIB`, 31-bit MCH) is a
   completely different mechanism with a different C runtime. The backend must
   `#error` when not `__MVS__ && _LP64` (XL C predefines `__MVS__`; `_LP64` /
   `__64BIT__` when compiling 64-bit; z/OS Clang also defines `__MVS__`).
2. **`POSIX(ON)` is required.** `sigaction()` and `pthread_kill()` are
   supported only in a POSIX program (z/OS UNIX), i.e. with the `POSIX(ON)`
   LE runtime option in effect. The library cannot force it; it can detect it
   (`ceeocb()->ceeocb_opt[__posix].ceeocb_opt_on`, see §"Runtime option
   detection") and must document the requirement.
3. **`TRAP(ON,SPIE)` (the default) gives signal delivery for program checks;
   exception handlers need `TRAP(ON)`/`TRAP(ON,NOSPIE)`.** With no exception
   handler registered, program checks and ABENDs map to SIGILL/SIGSEGV/SIGFPE/
   SIGABND and drive ordinary sigaction catchers, per the LE condition
   manager. The signal mask is ignored for program checks. `TRAP(ON,SPIE)` is
   also what OMR requires to consider "resume from signal handler" supported.
4. **A catcher driven by a program check/ABEND must not return.** With
   `POSIX(ON)`, returning from a catcher for SIGABND/SIGFPE/SIGILL/SIGSEGV
   that ran as a result of a program check or ABEND terminates the process.
   This is *not* the Linux/POSIX "return ⇒ re-execute the faulting
   instruction" model, and it is *not* the ISO-C model (where returning
   resumes at the instruction after the fault). Consequences:
   - `sig_decision_resume_execution` cannot be honoured for genuine program
     checks: "resume" would have to re-execute the faulting instruction,
     which z/OS cannot do from a POSIX catcher (a machine probe, Appendix A.5,
     determines whether `TRAP(ON,SPIE)`'s documented C-model resumability
     lets a sigaction catcher resume at the *next* instruction instead).
   - "Hand off to the previously installed handler" must be done by
     *re-raising* with the old disposition installed (SIG_DFL/SIG_IGN) rather
     than returning from our catcher after invoking the old handler; invoking
     a previously installed *function* handler directly is fine — if it
     returns, the process terminates exactly as it would have if that handler
     had been installed directly.
   - For async/software signals (SIGUSR1, SIGINT, SIGALRM, `raise()`...) the
     normal POSIX return semantics apply, so the shared machinery still works
     exactly as on Linux.
   - **Real-fault *ignore* is unimplementable for the four signals.** A
     SIG_IGN installed *directly* lets the LE enablement step ignore a
     program check before any catcher is driven; once the library's catcher
     replaces SIG_IGN, the check drives the catcher, and returning from it
     terminates the process. No catcher-side "resume and continue" exists in
     POSIX(ON) (the `_SA_OLD_STYLE` ISO-C model resumes at the next
     instruction but is explicitly the `signal()` compatibility mode and is
     rejected in §"Rejected designs"). The SIG_IGN re-raise-and-discard
     emulation in `invoke_sigaction` is therefore correct only for
     software/async raises; `siginstall()` on z/OS must document that it
     changes the effective disposition of SIGABND/SIGFPE/SIGILL/SIGSEGV for
     real faults.
   - z/OS defaults for SIGCHLD, SIGURG and SIGWINCH are "ignore" (Runtime
     Library Reference Table 57), so the POSIX backend's SIG_DFL-special-case
     for those signals is kept verbatim in the z/OS copy of `invoke_sigaction`.
5. **`siginfo_t` has no `si_addr`.** The common POSIX `prepare_rsi` reads
   `siginfo->si_addr`; that must be replaced on z/OS. The faulting address is
   available as the MCH *Breaking Event Address* via `__le_cib_get()`
   (`cib->cib_machine` → `__mch_bea`) whenever the catcher runs for a
   same-thread program check. `si_errno` is documented unused on z/OS, so
   expose `si_code` in `error_code` instead (a deliberate, documented
   deviation, see §`prepare_rsi`).
6. **Exception handlers are stack-frame scoped; a library cannot install a
   persistent one.** `__set_exception_handler()`'s registration dies with the
   registering frame (function return or longjmp past it). There is no
   process/thread-wide, unbounded-lifetime exception-handler registration in
   AMODE 64. This rules out an exception-handler-only backend for the global
   `siginstall()` machinery: the "previously installed handler" chain, global
   deciders, and `stdc_raise()` all require process-lifetime coverage. It
   *does* fit the `sigguarded()` per-frame model perfectly (see Phase 2).
7. **LE exception handlers cannot return, cannot resume, cannot percolate,
   and cannot un-register themselves.** Even where a frame-scoped handler is
   acceptable, it has one chance; "not handled" ⇒ abnormal termination. There
   is no documented way in AMODE 64 to re-drive the original program check,
   and `__reset_exception_handler()` cannot unregister the guard frame from
   inside the handler (it targets the *current* frame and fails EINVAL).
8. **z/OS signal numbering is non-POSIX** (`SIGABRT=3`, `SIGABND=18`,
   `SIGSEGV=11`, ..., `NSIG=42`; Appendix C), and there are z/OS-specific
   signals that cannot be caught or ignored at all: SIGDUMP(39), SIGTHSTOP(34),
   SIGTHCONT(35), SIGTRACE(37) ("cannot be caught or ignored; they always
   take effect"), plus SIGDANGER(33), SIGDCE(38), SIGMSG(36), SIGIOERR(27)
   etc. `siginstall(NULL)` fills all 42 bits, so the install loops must skip
   the uncatchables or `sigaction()` fails and installation aborts.
9. **`siginfo_t`/`ucontext_t` availability.** z/OS provides both `siginfo_t`
   (`<siginfo.h>`) and `ucontext_t` (`<ucontext.h>`), and the sa_sigaction
   third argument points at a ucontext (OMR reinterprets it as the machine
   context `__mcontext_t_` from z/OS header `edcwccwi.h`, which overlays it;
   risk 12). To avoid any dependence on the `siginfo_t` struct *tag*
   spelling, the header's `__MVS__` branch aliases the type directly:
   `typedef siginfo_t stdc_siginfo_siginfo_t` (the same approach the
   `__FILC__` branch uses).
10. **Async-signal-safe TLS.** Classic XL C is not GCC/MSVC, so
    `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` already defaults to 0 and the
    tss_async_signal_safe fallback is used — the correct choice on z/OS (LE
    TLS semantics under a signal are not documented). z/OS Clang *does* define
    `__GNUC__`, so the plan must force the fallback for `__MVS__` regardless
    (config.h change).
11. **`current_thread_id.c.ipp` hard-fails on z/OS** (`#error "unsupported
    platform"`). Needs a `__MVS__` branch (`pthread_self()` value).
12. **`__le_cib_get()` availability in catchers.** Valid in a POSIX(ON)
    catcher only when the signal was generated and caught immediately on the
    same thread. Exactly our program-check case; it may return NULL otherwise
    (e.g. async signals) — handle NULL gracefully.
13. **setjmp/longjmp sizing (XPLINK).** jmp_buf/sigjmp_buf/ucontext_t layout
    changed at LE V2R10, and differs between XPLINK and non-XPLINK
    compilation. The library must be built consistently (same LE headers and
    linkage as the consuming program); the plan documents the constraint, no
    code change needed beyond using `sigsetjmp`/`siglongjmp`.

## Design

### Recommended design (Phase 1): signals-only backend mirroring the POSIX backend

The z/OS backend is the POSIX backend (`thrd_signal_handle_posix.c.ipp`)
with a thin z/OS adaptation layer. This is what production z/OS software
(J9/OMR, Go) actually does: plain `sigaction(SA_SIGINFO)` catchers, with LE
generating the signals for program checks, plus LE services used *from inside
the catcher* to enrich the `stdc_siginfo`. Rationale:

- It is the *only* design that can implement the full public API:
  `siginstall()`'s global, process-lifetime, previously-installed-handler
  chaining is impossible with frame-scoped LE exception handlers (fact 6).
- The LE condition manager already delivers same-thread synchronous signals
  to the incurring thread, ignoring the signal mask for program checks
  (fact 3) — matching the library's "synchronous signals are delivered to
  the thread that caused them" model.
- `sigguarded()`/`stdc_raise()` frame chains, global deciders, and the
  reference-counting in `thrd_signal_handle_common.ipp.ipp` are reused
  unchanged; only the platform leaves (`prepare_rsi`, `invoke_sigaction`,
  `install_sighandler_impl`, `uninstall_sighandler_impl`, the sigfillset_*
  tables, and the setjmp choice) differ.
- The known deviations from Linux behaviour are confined to two documented
  points (both are *platform* properties, not library bugs):
  a. `resume_execution` on a genuine program-check-derived SIGABND/SIGFPE/
     SIGILL/SIGSEGV cannot re-execute the faulting instruction; the decider's
     request is documented as "unimplementable on z/OS; the catcher returns
     and z/OS terminates the process" (or, for `TRAP(ON,SPIE)` verified
     machines, resumes at the *next* instruction — machine probe §A.5
     resolves which).
  b. When no decider claims a real program-check signal, the previously
     installed action is emulated by *re-raise* (re-installing the old
     disposition and `pthread_kill`-ing the thread), never by returning from
     our catcher, because returning terminates the process (fact 4).

### Rejected designs

- **Exception-handler-only backend.** Impossible (facts 6, 7): no
  process-lifetime registration, no "previous handler" chain, no resume, no
  percolation. Also `TRAP(ON,NOSPIE)` changes global runtime behaviour the
  library cannot restore.
- **`_SA_OLD_STYLE` (ISO-C delivery) instead of `SA_SIGINFO`.** The ISO-C
  model gives return-and-resume-at-next-instruction semantics and the
  `__le_cib_get()` guarantee, but loses `siginfo_t`/`ucontext_t`, per-signal
  masks, and the per-thread (POSIX) delivery model, and is explicitly
  discouraged ("sigaction() is the strategic way"). Kept as a fallback only
  if machine probing (A.5) shows `_SA_OLD_STYLE` is needed to make
  `resume_execution` work for program checks.
- **Pure LE-exception-handler sigguarded() (Phase 2 without signals).**
  Even for guards, the frame-scoped handler cannot hand unclaimed conditions
  to the previously installed handler or to global deciders installed
  outside the guard. Phase 2 below keeps signals as the base and uses the
  exception handler only as a delivery backstop.

### Phase 2 (optional, later): LE exception-handler backstop for `sigguarded()`

**Confirmed design — the exception handler is a pure "program check →
software-signal bridge".** It does *not* dispatch deciders itself. It maps
the CIB to a signo and re-raises that signo as a *software* signal via
`pthread_kill()`; the normal sigaction catcher then runs the standard
`stdc_raise()` dispatch (frame walk, global deciders, previously-installed
handler) with true POSIX, returnable semantics. Rationale:

- A software signal is not a program check, so it can never re-enter the
  (suspended) exception handler (a6eh: only program checks/ABENDs drive
  exception handlers; kill-generated signals follow pure POSIX delivery), so
  the bridge cannot recurse.
- Dispatching deciders *inside* the handler and *also* re-raising would
  double-invoke every side-effecting decider (the exact problem the Windows
  backend's V5 dedup solves); the bridge dispatches exactly once.
- The guard's recovery still works: the catcher's `siglongjmp()` to the
  guard buffer unwinds past the suspended handler frame, so control never
  returns to the handler (and LE removes the handler registration normally).
- If the re-raise is discarded or unhandled and control returns to the
  handler, the handler calls `__cabend()` — an unhandled guarded program
  check terminating is the documented LE outcome.
- Under the default `TRAP(ON,SPIE)` the handler is simply never driven (or
  registration fails harmlessly); under `TRAP(ON,NOSPIE)` it is what makes
  guarded program checks observable at all. The bridge therefore works in
  both TRAP modes with identical semantics; TRAP detection becomes a
  diagnostic only (below).

Because the handler is registered on the `sigguarded()` frame it is
auto-scoped exactly like the guard frame itself (auto-deregistration on
guard return and on longjmp past the guard — the a6eh "scope and nesting"
semantics line up 1:1 with the frame chain).

Note on the termination path: OMR observes (port/unix/omrsignal.c) that when
a hardware condition is ultimately unhandled, returning from the catcher
keeps the *original program check* visible in LE diagnostics as the cause,
whereas re-raising as a software signal (or `__cabend()`) makes LE report the
re-raise as the cause. The bridge's termination path therefore reports a
software SIGSEGV instead of the original check; harmless, but worth a comment
in the code and a line in the documentation.

```c
#if WG14_SIGNALS_ZOS_USE_LE_EXCEPTION_HANDLERS
// Stash for the faulting address of a bridged program check. The bridge has
// the CIB (and thus the MCH Breaking Event Address) but the re-raised
// software signal does not, and __le_cib_get() returns NULL in the catcher
// for a software signal; zos_fault_address() falls back to this slot so
// rsi->addr keeps its fidelity through the bridge (see §prepare_rsi).
static WG14_SIGNALS_THREAD_LOCAL void *WG14_SIGNALS_PREFIX(zos_bridge_addr);

// LE invokes this for program checks and ABENDs anywhere below the
// registering sigguarded() frame, INSTEAD of generating the mapped signal
// (a6eh). It must never return. It is a bridge: convert the CIB to a signo
// and re-raise it as a software signal so the normal sigaction catcher runs
// the full stdc_raise() dispatch. If control ever returns here (the re-raise
// was discarded/unhandled), abnormal termination is the documented "did not
// handle" outcome.
  static void WG14_SIGNALS_PREFIX(zos_exception_handler)(struct __cib *cib,
                                                         void *user_data)
  {
    (void) user_data;
    const int signo = WG14_SIGNALS_PREFIX(zos_signo_from_cib)(cib);
    if(signo != 0)
    {
      // Preserve the faulting address across the software re-raise.
      WG14_SIGNALS_PREFIX(zos_bridge_addr) =
      WG14_SIGNALS_PREFIX(zos_fault_address_from_cib)(cib);
      // Software raise: never re-enters this (suspended) handler; the
      // sigaction catcher's siglongjmp to a recovery buffer unwinds past this
      // frame, so control returns here only if the signal path completed.
      (void) pthread_kill(pthread_self(), signo);
    }
    __cabend(0xEC1 /*WG14_SIGNALS*/, 0, 0);  // never returns
  }
#endif
```

Registration inside `sigguarded()`, after `tss->front = &current` and
*before* calling `guarded()`; failure of `__set_exception_handler`
(-1/EINVAL, e.g. under `TRAP(ON,SPIE)`) is non-fatal — signal-only coverage
remains. Since LE forbids registering more than one exception handler per
stack frame, never register twice per guard (the backend doesn't).

Remaining open points:
- **CIB→signo mapping**: reuse the `__set_exception_handler` documentation
  table (Appendix B), mapping the CIB feedback code to SIGILL/SIGSEGV/SIGFPE/
  SIGABND. The exact CIB field carrying the feedback code (message number /
  `__cib_fc` / `cib_icdi`-style field) must be confirmed on the machine
  (Appendix A.4) — the a6eh table is authoritative for the mapping, the
  struct layout is in `<__le_cib.h>`.
- **`__reset_exception_handler()` is unusable from inside the handler**
  (confirmed: it targets the current stack frame and fails EINVAL), which is
  exactly why the bridge does not try to unregister before re-raising.

## Repository integration (file-by-file)

1. `include/wg14_signals/detail/impl/thrd_signal_handle_zos.c.ipp` — new
   backend (sketch below).
2. `src/wg14_signals/thrd_signal_handle_zos.c` — new one-line wrapper
   (mirrors `thrd_signal_handle_posix.c`).
3. `include/wg14_signals/thrd_signal_handle.h`:
   - header-only include selection (lines 641-647):
     ```c
     #if WG14_SIGNALS_ENABLE_HEADER_ONLY
     #ifdef _WIN32
     #include "detail/impl/thrd_signal_handle_windows.c.ipp"
     #elif defined(__MVS__)
     #include "detail/impl/thrd_signal_handle_zos.c.ipp"
     #else
     #include "detail/impl/thrd_signal_handle_posix.c.ipp"
     #endif
     #endif
     ```
   - `stdc_siginfo_siginfo_t` typedef (lines 359-373): add
     `#elif defined(__MVS__)` before the existing branches,
     ```c
     #elif defined(__MVS__)
     // z/OS defines siginfo_t in <siginfo.h>; alias it directly (like the
     // __FILC__ branch) so we never depend on the struct tag spelling.
     typedef siginfo_t WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
     ```
   - `stdc_siginfo_context_t` (lines 375-381): the non-Windows
     `typedef ucontext_t ...` branch already works (`<ucontext.h>` exists,
     the sa_sigaction third argument points at it); no change. The
     `stdc_siginfo_error_code_t` non-Windows `int` branch (line 354)
     fits `si_code`; no change.
4. `include/wg14_signals/detail/impl/thrd_signal_handle_common.ipp.ipp`:
   - the per-frame `buf` member becomes `sigjmp_buf` under `__MVS__`
     (line 265; required because the z/OS setjmp pair is
     sigsetjmp/siglongjmp, and `sigjmp_buf` is a distinct, larger type than
     `jmp_buf` on z/OS — see the ordered task list item 3a for the exact
     guard);
   - the six `#ifdef __MVS__` uncatchable-signal skips (§"Skip the
     uncatchable z/OS signals in the install loops").
5. `include/wg14_signals/config.h` — force the fallback TLS on z/OS (line 56):
   ```c
   #if (defined(__GNUC__) || defined(_MSC_VER)) && !defined(__APPLE__) \
       && !defined(__MVS__)
   #define WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL 1
   #else
   #define WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL 0
   #endif
   ```
   (z/OS Clang defines `__GNUC__`; XL C does not. Either way z/OS must use
   the tss_async_signal_safe fallback: LE does not document async-signal-safe
   TLS, and the fallback needs only pthread_key + `thread_atexit`
   (pthread_key path) + `current_thread_id()`, all available.)
6. `include/wg14_signals/detail/impl/current_thread_id.c.ipp` — add a
   `__MVS__` branch (the `#error` is at lines 86-89):
   ```c
   #elif defined(__MVS__)
   return (WG14_SIGNALS_PREFIX(thread_id_t)) pthread_self();
   ```
   (`#include <pthread.h>` under `__MVS__` alongside the existing
   per-platform includes; `pthread_t` on z/OS is an `unsigned long`, unique
   per thread — same contract as Linux's `gettid`. Note this .ipp is also
   pulled into **header-only consumer TUs** via `current_thread_id.h:86`,
   so the branch is mandatory for header-only z/OS builds, not just the
   library, and the `pthread.h` include must be C++-clean like the existing
   FreeBSD `pthread_np.h` include. Without the branch, a header-only z/OS
   compile fails at the `#error` before the backend is even reached.)
7. `CMakeLists.txt`:
   - **Detect z/OS once, near the top** (after `project()`), hedging the
     system-name spelling (CMake's z/OS platform support, added in 3.22, has
     been spelled both `zos` and `zOS` across releases; the PLATFORM_ID
     genex has the same ambiguity). All later branches use the variable:
     ```cmake
     # CMake's z/OS platform support (>= 3.22) spells the system name both
     # "zos" and "zOS" across releases; accept either. Document that z/OS
     # builds need CMake >= 3.22.
     if(CMAKE_SYSTEM_NAME STREQUAL "zos" OR CMAKE_SYSTEM_NAME STREQUAL "zOS")
       set(WG14_SIGNALS_ZOS TRUE)
     endif()
     ```
   - library sources (lines 85-91): replace the NOT-Windows genex with an
     explicit three-way backend selection (no PLATFORM_ID spelling risk):
     ```cmake
     if(WIN32)
       set(WG14_SIGNALS_BACKEND_SOURCE "src/wg14_signals/thrd_signal_handle_windows.c")
     elseif(WG14_SIGNALS_ZOS)
       set(WG14_SIGNALS_BACKEND_SOURCE "src/wg14_signals/thrd_signal_handle_zos.c")
     else()
       set(WG14_SIGNALS_BACKEND_SOURCE "src/wg14_signals/thrd_signal_handle_posix.c")
     endif()
     set(LIBRARY_SOURCES
       "src/wg14_signals/current_thread_id.c"
       "src/wg14_signals/tss_async_signal_safe.c"
       "${WG14_SIGNALS_BACKEND_SOURCE}"
       "${THREAD_ATEXIT_SOURCE}"
     )
     ```
   - warning flags (lines 188-191): insert a z/OS branch between MSVC and
     the GCC flags:
     ```cmake
     if(MSVC)
       target_compile_options(${PROJECT_NAME} PRIVATE /W4 /experimental:c11atomics)
     elseif(WG14_SIGNALS_ZOS)
       if(CMAKE_C_COMPILER_ID MATCHES "Clang")
         target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
       else()
         # XL C has no GCC-style -W flags; use its own message/warning control
         # (warnings to W file, not fatal).
         target_compile_options(${PROJECT_NAME} PRIVATE -qflag=w:w -qhalt=s)
       endif()
     else()
       target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
     endif()
     ```
     The same branch pattern is needed in the `add_code_example` /
     `add_code_test` helper if it sets warning flags.
   - feature-test macros (lines 129-164): **skip the generic ladder on
     z/OS** and set the z/OS-specific set instead. z/OS feature-test macros
     are mutually exclusive (Runtime Library Reference, "Header files —
     Feature test macros"): `_OPEN_THREADS` and `_UNIX03_THREADS` are
     mutually exclusive and both conflict with `_XOPEN_SOURCE 600`;
     `_GNU_SOURCE` is meaningless on XL C; `_XOPEN_SOURCE=700` from the
     generic ladder would contradict z/OS's `_XOPEN_SOURCE_EXTENDED`. The
     documented set for this backend's API is `_POSIX_SOURCE`,
     `_XOPEN_SOURCE_EXTENDED 1` (sigaction) and `_OPEN_THREADS 1`
     (pthread_kill/pthread_self):
     ```cmake
     if(WG14_SIGNALS_ZOS)
       target_compile_definitions(${PROJECT_NAME} PRIVATE
         _POSIX_SOURCE
         _XOPEN_SOURCE_EXTENDED=1
         _OPEN_THREADS=1)
     elseif(NOT MSVC)
       ... existing ladder ...
     endif()
     ```
     The .ipp also defines these itself before the first system header so
     header-only consumers compile identically (see skeleton). Note for
     compiler selection: the common machinery requires C11 `<stdatomic.h>`
     (atomic_uint, atomic_thread_fence); XL C on z/OS 3.x supports C11 under
     the `LANGLVL(EXTENDED)`/z/OS Clang compilers — a build-time
     `check_c_source_compiles` for `<stdatomic.h>` should gate the error if
     the toolchain is too old.
   - `LIBC_HAS__SETJMP` probe (lines 176-178): fine as-is (`_setjmp()` exists
     on z/OS; the backend does not use it, see §setjmp).
   - `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` probe (lines 78-107): on z/OS the
     symbol lives in the C++ runtime (libC++), not the LE C runtime, so the
     C-compiled probe is expected to fail → the pure-POSIX `pthread_key`
     fallback in `thread_atexit.c.ipp` is used (the ideal outcome: no C++
     dependency in a C library). If it unexpectedly passes, the library must
     link the C++ runtime: mirror the FreeBSD `libstdthreads` handling at
     lines 108-122 and set `WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB` to the z/OS
     C++ runtime library (e.g. `c++`). Header-only consumers get the same
     choice via `thread_atexit.h:37-50` (C TU → pthread_key path; C++ TU →
     the C++ implementation if the probe failed).
   - the `--wrap=calloc`-based tests and the assembly-inspection
     `sigfence_codegen_test` are GNU-ld/x86 specific; exclude them on z/OS
     like the MSVC/Apple/Fil-C exclusions.
   - `test/CMakeLists.txt`: no z/OS runner exists; nothing to add beyond the
     exclusions above (the `siginstall_rollback_test` guard at
     `test/CMakeLists.txt:24` is the model: extend it with the z/OS
     condition).
8. `.github/workflows/ci.yml` — no public z/OS runners; document that z/OS
   validation is manual (Appendix A). Optionally add a "zos" compile-only
   job later if a z/OS cross toolchain becomes available (IBM Wazi/ADCD
   images are not CI-viable today).
9. Documentation: `Readme.md` platform table, `plans/ideas.md` (§2 header
   techniques: z/OS `#error` guard pattern, `__MVS__` platform branch),
   `plans/analysis.md` new-finding rows (see §"Documentation updates").

## Implementation

### Skeleton of `thrd_signal_handle_zos.c.ipp`

```c
#ifndef WG14_SIGNALS_THRD_SIGNAL_HANDLE_ZOS_IPP
#define WG14_SIGNALS_THRD_SIGNAL_HANDLE_ZOS_IPP

// A wrong-platform include is a clear compile error instead of a silent
// mis-compile (plans/ideas.md 4.4). AMODE 64 only: the 31-bit model
// (CEEHDLR, USRHDLR, CEE3CIB) is a different runtime entirely.
#if !defined(__MVS__) || !defined(_LP64)
#error "thrd_signal_handle_zos.c.ipp must only be included on AMODE 64 z/OS"
#endif

// z/OS feature-test discipline (Runtime Library Reference): sigaction() needs
// _POSIX_SOURCE and _XOPEN_SOURCE_EXTENDED; pthread_kill() needs
// _OPEN_THREADS (or _UNIX03_THREADS under SUSV3). Define them here, before
// any z/OS system header, so header-only consumers compile identically.
#ifndef _POSIX_SOURCE
#define _POSIX_SOURCE 1
#endif
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif
#ifndef _OPEN_THREADS
#define _OPEN_THREADS 1
#endif

#include "../../thrd_signal_handle.h"

#include <pthread.h>
#include <signal.h>
#include <setjmp.h>

// Phase 1 requires __le_cib_get() (to extract the faulting address into
// rsi->addr); it is declared in <__le_api.h> (AMODE 64 LE API header,
// present on z/OS 1.13+). Phase 2 additionally needs the CIB/MCH structure
// headers. If WG14_SIGNALS_ZOS_USE_LE_CIB is defined 0, the z/OS
// prepare_rsi falls back to addr = NULL (see §prepare_rsi).
#include <__le_api.h>

// Phase 2 only (see "Phase 2" section); the Phase 1 file must compile without
// these:
//#include <__le_cib.h>   // struct __cib (cib_pcf, cib_abf, cib_machine)
//#include <__le_mch.h>   // struct __mch (__mch_bea)

#include "thrd_signal_handle_common.ipp.ipp"

#include "linked_list.h"

#ifdef __cplusplus
extern "C"
{
#endif

// sigsetjmp()/siglongjmp() restore the signal mask on recovery (the LE docs
// list siglongjmp explicitly among the valid exits from an exception handler;
// it is also the pair OMR uses for z/OS recovery). The common code calls
// WG14_SIGNALS_SETJMP(buf) with one argument, so sigsetjmp's savemask flag is
// folded in here (always save the mask: a recovery routine may raise signals).
#define WG14_SIGNALS_SETJMP(buf) sigsetjmp((buf), 1)
#define WG14_SIGNALS_LONGJMP siglongjmp
```

Note: `setjmp` is a macro on z/OS XL C; `sigsetjmp` is declared in
`<setjmp.h>` with `_POSIX_SOURCE` defined (the XL C Language Reference lists
`setjmp`, `_setjmp`, `sigsetjmp` and `_sigsetjmp` as the setjmp family, so
`_sigsetjmp(env, savemask)` is the underscore-form fallback if a build
environment ever shadows `sigsetjmp`). The `WG14_SIGNALS_SETJMP` wrapper
must be a function-like macro exactly as above, defined after all includes,
before use. It also requires the common-include frame-buffer change to
`sigjmp_buf` under `__MVS__` (ordered task list item 3a): `sigsetjmp` and
`siglongjmp` operate on `sigjmp_buf`, which on z/OS is a distinct and
*larger* type than the `jmp_buf` the per-frame struct declares today.

### The three sigfillset_* tables (lazy-init pattern of the POSIX backend)

The POSIX file marks these `static __attribute__((constructor))` so the sets
are primed at load time, before any signal can observe the lazy-init race.
z/OS XL C supports the `__attribute__` keyword but GNU `(constructor)` is
unconfirmed (z/OS Clang supports it); in the z/OS copies guard it with
`#if defined(__GNUC__) || defined(__clang__)` and otherwise rely on the
lazy init — `sigset_t` on z/OS is a single 64-bit `unsigned long`, so the
benign same-content re-init race is a non-issue. Verify on the machine if
XL C accepts the attribute; the probe (Appendix A.1) prints it.

```c
  static const sigset_t *WG14_SIGNALS_PREFIX(synchronous_sigset)(void)
  {
    static sigset_t v;
    // z/OS synchronous signals: the S/370 program checks (Table 16) plus
    // abends (SIGABND, Table 17), abort() (SIGABRT) and the POSIX
    // synchronous set the header documents. All exist on z/OS
    // (Appendix C). SIGABND is the IBM-specific "an ABEND occurred" signal.
    static const int signos[] = {
      SIGABRT, SIGABND, SIGBUS, SIGFPE, SIGILL, SIGPIPE, SIGSEGV, SIGSYS};
    if(sigismember(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    sigemptyset(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      sigaddset(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_synchronous)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(synchronous_sigset)(), sizeof(*set));
    return 0;
  }
```

```c
  static const sigset_t *WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(void)
  {
    static sigset_t v;
    // Every member is in the z/OS UNIX signal list (Table 57, Appendix C).
    static const int signos[] = {
      SIGALRM, SIGCHLD, SIGCONT, SIGHUP,  SIGINT,  SIGKILL, SIGSTOP,
      SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGUSR1, SIGUSR2, SIGPOLL,
      SIGPROF, SIGURG,  SIGVTALRM};
    if(sigismember(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    sigemptyset(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      sigaddset(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_nondebug)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(),
           sizeof(*set));
    return 0;
  }

  static const sigset_t *WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {SIGQUIT, SIGTRAP, SIGXCPU, SIGXFSZ};
    if(sigismember(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    sigemptyset(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      sigaddset(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_debug)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(), sizeof(*set));
    return 0;
  }
```

All three are async-signal-safe (lazy static init outside the handler, pure
sigset ops) exactly as on POSIX.

### `prepare_rsi`: no `si_addr`; use the CIB/MCH Breaking Event Address

```c
#if WG14_SIGNALS_ZOS_USE_LE_CIB
// Extract the faulting instruction address (MCH Breaking Event Address)
// from a CIB the caller already holds (used by the Phase 2 bridge, which
// receives the CIB as its parameter).
  static void *WG14_SIGNALS_PREFIX(zos_fault_address_from_cib)(
  struct __cib *cib)
  {
    if(cib != WG14_SIGNALS_NULLPTR &&
       (cib->cib_pcf == 1 || cib->cib_abf == 1))
    {
      struct __mch *mch = (struct __mch *) cib->cib_machine;
      if(mch != WG14_SIGNALS_NULLPTR)
      {
        return (void *) (uintptr_t) mch->__mch_bea;
      }
    }
    return WG14_SIGNALS_NULLPTR;
  }

// Called from a catcher for a same-thread signal (the case __le_cib_get()
// documents as valid). Returns the faulting instruction address (Breaking
// Event Address) when the condition really is a program check or abend,
// else NULL (e.g. async or software signals). Falls back to the Phase 2
// bridge's thread-local stash: under TRAP(ON,NOSPIE) a bridged program
// check reaches this catcher as a *software* re-raise, which carries no
// CIB, so the bridge records the BEA in the stash before re-raising.
  static void *WG14_SIGNALS_PREFIX(zos_fault_address)(void)
  {
    void *addr = WG14_SIGNALS_PREFIX(zos_fault_address_from_cib)(
    __le_cib_get());
    if(addr == WG14_SIGNALS_NULLPTR)
    {
#if WG14_SIGNALS_ZOS_USE_LE_EXCEPTION_HANDLERS
      addr = WG14_SIGNALS_PREFIX(zos_bridge_addr);
#endif
    }
    return addr;
  }
#else
  static void *WG14_SIGNALS_PREFIX(zos_fault_address)(void)
  {
    return WG14_SIGNALS_NULLPTR;
  }
#endif

  static void WG14_SIGNALS_PREFIX(prepare_rsi)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi, const int signo,
  WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * siginfo,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * context)
  {
    rsi->signo = signo;
    rsi->raw_context = context;
    if(siginfo != WG14_SIGNALS_NULLPTR)
    {
      rsi->raw_info = siginfo;
      // si_errno is documented as "not used on this implementation"
      // (Runtime Library Reference, sigaction()); si_code is set (SI_USER,
      // SI_KERNEL, and the per-signal SIG*_* cause codes). Expose si_code as
      // the "error code", and the MCH Breaking Event Address as addr. The
      // original siginfo_t remains available via raw_info for deciders that
      // want si_code/si_pid/si_uid.
      rsi->error_code = siginfo->si_code;
      rsi->addr = WG14_SIGNALS_PREFIX(zos_fault_address)();
    }
    else
    {
      // stdc_raise(signo, NULL, NULL) must hand deciders deterministic
      // "no OS info" values (analysis.md 2.14/W2); do NOT memset the whole
      // struct, `value` is pre-set on the frame path.
      rsi->raw_info = WG14_SIGNALS_NULLPTR;
      rsi->error_code = 0;
      rsi->addr = WG14_SIGNALS_NULLPTR;
    }
  }
```

`WG14_SIGNALS_ZOS_USE_LE_CIB` defaults to 1 (the `<__le_api.h>` include is
needed); it exists only so a build can disable LE services if a header is
missing on some z/OS release. `WG14_SIGNALS_ZOS_USE_LE_EXCEPTION_HANDLERS`
(Phase 2, defaults to 1) must be disabled together with
`WG14_SIGNALS_ZOS_USE_LE_CIB`: the bridge's BEA stash calls
`zos_fault_address_from_cib()`, which is compiled only under the latter
guard. Field names (`cib_pcf`, `cib_abf`,
`cib_machine`, `__mch_bea`) are exactly those used by the OMR z/OS port;
confirm against `<__le_cib.h>`/`<__le_mch.h>` on the machine (Appendix A.4).

### `raw_signal_handler` and `invoke_sigaction` (the z/OS semantic core)

```c
  // The base signal handler for z/OS. You must NOT do anything async signal
  // unsafe in here!
  static void WG14_SIGNALS_PREFIX(raw_signal_handler)(int signo,
                                                      siginfo_t *siginfo,
                                                      void *context)
  {
    if(!WG14_SIGNALS_PREFIX(stdc_raise)(
       signo, siginfo, (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) context))
    {
      // No guard and no decider claimed the signal: hand off to the
      // previously installed action via a fresh dispatch (never by simply
      // returning from a program-check-driven catcher, which terminates the
      // process on z/OS).
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = SIG_DFL;
      WG14_SIGNALS_PREFIX(invoke_sigaction)(&sa, signo, siginfo, context);
    }
  }

  // Invoke a sigaction as if it were the first signal handler.
  static bool WG14_SIGNALS_PREFIX(invoke_sigaction)(const struct sigaction *sa,
                                                    const int signo,
                                                    siginfo_t *siginfo,
                                                    void *context)
  {
    if((sa->sa_flags & SA_SIGINFO) != 0)
    {
      // The previously installed SA_SIGINFO handler. We invoke it directly;
      // if it returns while our catcher is still on the stack and this was a
      // program-check/ABEND-driven SIGABND/SIGFPE/SIGILL/SIGSEGV, z/OS
      // terminates the process — exactly what the old handler would have
      // experienced itself, so behaviour is preserved.
      sa->sa_sigaction(signo, siginfo, context);
      return true;
    }
    if(sa->sa_handler == SIG_DFL)
    {
      // z/OS defaults for SIGCHLD, SIGURG and SIGWINCH are "ignore" (Table
      // 57), exactly as on the POSIX backend: do nothing (return false; the
      // caller treats "unclaimed" like "handled").
      switch(signo)
      {
      case SIGCHLD:
      case SIGURG:
#ifdef SIGWINCH
      case SIGWINCH:
#endif
        return false;
      default:
      {
        // Default action: reset the disposition and re-send the signal to
        // this thread so LE applies the default (termination; for SIGABND
        // with dump). This avoids returning from our own catcher, which the
        // POSIX(ON) rules forbid for program-check-driven deliveries.
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        (void) sigaction(signo, &dfl, WG14_SIGNALS_NULLPTR);
        (void) pthread_kill(pthread_self(), signo);
        return true;
      }
      }
    }
    if(sa->sa_handler == SIG_IGN)
    {
      // Emulate "ignore": install SIG_IGN and re-send the signal to this
      // thread. Setting SIG_IGN for a signal that is pending discards it
      // (Runtime Library Reference, sigaction()), so LE never drives any
      // catcher and execution continues. Correct for software/async raises.
      // For a REAL program check/ABEND of SIGABND/SIGFPE/SIGILL/SIGSEGV this
      // is best-effort only: LE drove our catcher because a catcher was
      // installed; once driven, returning from it terminates the process
      // regardless of disposition (the documented RETURN rule), and there is
      // no documented way to "resume and continue" from a POSIX(ON) catcher.
      // siginstall() therefore changes the effective disposition of those
      // four signals for real faults — a documented z/OS deviation (fact 4).
      struct sigaction ign;
      memset(&ign, 0, sizeof(ign));
      ign.sa_handler = SIG_IGN;
      (void) sigaction(signo, &ign, WG14_SIGNALS_NULLPTR);
      (void) pthread_kill(pthread_self(), signo);
      return true;
    }
    sa->sa_handler(signo);
    return true;
  }
```

Notes:
- The POSIX backend's `SIGCHLD`/`SIGURG`/`SIGWINCH` "default is to ignore"
  special cases are kept: on z/OS their documented default action is
  "ignore" too (Table 57). The `SIG_IGN` *re-raise-and-discard* emulation is
  z/OS-specific (a plain `return false` would be correct on POSIX, where a
  catcher may return freely, but on z/OS returning from a
  program-check-driven catcher terminates the process).
- **Real-fault *ignore* is unimplementable on z/OS POSIX(ON)** for the four
  program-check/ABEND signals: while SIG_IGN installed *directly* lets the
  LE enablement step ignore a program check before any catcher is driven,
  once the library's catcher replaces SIG_IGN the check drives the catcher,
  and returning from it terminates the process (documented rule) — the
  re-raise-and-discard above cannot restore ignore for that case. This is a
  platform property (any library or app replacing SIG_IGN has the same
  problem) and must be documented on `siginstall()` for z/OS; it does not
  affect software/async raises, and recovery-via-guard still works.
- `pthread_kill(pthread_self(), signo)` requires `_OPEN_THREADS`
  (defined at the top). `raise(signo)` would be equivalent for the
  default/ignore emulation; pthread_kill keeps the POSIX-backend form and is
  documented to deliver to the calling thread.

### `stdc_raise` and `sigguarded`

`sigguarded()` and `stdc_raise()` are *not* in the common include: each
backend re-states them (the Windows backend has its own SEH-based
`sigguarded()` and RaiseException-based `stdc_raise()` at
`thrd_signal_handle_windows.c.ipp:285-410`). The z/OS file therefore copies
`thrd_signal_handle_posix.c.ipp:242-410` **verbatim** — the whole
`sigguarded()` frame-chain setup and the whole `stdc_raise()` frame-walk →
global-decider chain → `invoke_sigaction` fallback are byte-identical (they
only use common structures: the per-thread TSS, `struct
sig_global_state_tss_state_per_frame_t`, `prepare_rsi`, `invoke_sigaction`,
`WG14_SIGNALS_SETJMP/LONGJMP`). The only z/OS deltas inside those copies:

- `WG14_SIGNALS_SETJMP/LONGJMP` resolve to the sigsetjmp/siglongjmp pair
  (handled by the defines above). This requires the one common-include
  change: `sig_global_state_tss_state_per_frame_t.buf` must be `sigjmp_buf`
  under `__MVS__` (the field is `jmp_buf` today; `sigsetjmp`/`siglongjmp`
  take the distinct, larger `sigjmp_buf` on z/OS — passing a `jmp_buf`
  would be a type error, and casting would overflow the smaller buffer).
  `sigguarded()` additionally registers the Phase 2 LE exception-handler
  bridge at the single hook point shown below.
- `prepare_rsi` is the z/OS one above.
- The `sig_decision_resume_execution` branch is identical (return `true`);
  its z/OS caveat is documented in the header (and probe A.5 decides whether
  `TRAP(ON,SPIE)` machines resume at the next instruction instead).
- The final fall-through calls the z/OS `invoke_sigaction` (above).

Annotated z/OS `sigguarded()` (the Phase 2 hook is the only addition to the
POSIX copy, inside `#if WG14_SIGNALS_ZOS_USE_LE_EXCEPTION_HANDLERS`):

```c
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
  WG14_SIGNALS_PREFIX(sigguarded)(const sigset_t *signals,
                                  WG14_SIGNALS_PREFIX(sig_func_t) guarded,
                                  WG14_SIGNALS_PREFIX(sig_recover_t) recovery,
                                  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
                                  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
                                  value)
  {
    if(signals == WG14_SIGNALS_NULLPTR || guarded == WG14_SIGNALS_NULLPTR ||
       decider == WG14_SIGNALS_NULLPTR)
    {
      abort();
    }
    if(0 != WG14_SIGNALS_PREFIX(sig_global_tss_state_init)())
    {
      union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret;
      ret.int_value = -1;
      return ret;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *old =
    tss->front,
                                                                       current;
    memset(&current, 0, sizeof(current));
    current.prev = old;
    current.guarded = signals;
    current.recovery = recovery;
    current.decider = decider;
    current.rsi.value = value;
    tss->front = &current;
#if WG14_SIGNALS_ZOS_USE_LE_EXCEPTION_HANDLERS
    // HOOK: register the bridge on THIS frame. The registration lives on
    // sigguarded()'s stack frame, so LE scope/nesting gives it exactly the
    // guard's lifetime: auto-removed when this frame returns, and when a
    // longjmp jumps back *past* it (e.g. an outer guard's recovery). A
    // longjmp landing INSIDE this frame (a decider's invoke_recovery) keeps
    // it registered, which is intended: recovery() then still gets bridged
    // program-check coverage under TRAP(ON,NOSPIE), while tss->front has
    // already been popped (below), so recovery-raised faults dispatch to the
    // global deciders / previously-installed handler and never re-enter this
    // guard's decider — exactly the POSIX design's "previous handler active
    // before recovery" intent. No explicit __reset_exception_handler() is
    // ever needed here (and from inside a driven handler it would fail with
    // EINVAL anyway). Failure of registration (EINVAL, e.g. under
    // TRAP(ON,SPIE), or "too many handlers on this frame") is non-fatal —
    // signal-only coverage remains.
    const int _eh_rc = __set_exception_handler(
    WG14_SIGNALS_PREFIX(zos_exception_handler), WG14_SIGNALS_NULLPTR);
    (void) _eh_rc;
#endif
    if(WG14_SIGNALS_SETJMP(current.buf) != 0)
    {
      tss->front = old;
      // Technically needed to ensure previous handler is active before recovery
      // function is called, as it may raise a signal
      atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
      return recovery(&current.rsi);
    }
    // Technically needed to ensure setjmp buffer written out before guarded
    // function is called
    atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret = guarded(value);
    tss->front = old;
    atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
    return ret;
  }
```

Why there is no dismantle hook: every exit path is already covered by LE
auto-deregistration (frame return, and longjmp past the frame), and the one
case where the bridge legitimately outlives the guarded() call — the
recovery() phase after a decider's longjmp — is exactly the case where it
should stay (see HOOK comment). Explicit resets would either be redundant
(normal return) or harmful (removing the bridge before recovery() would make
recovery-time program checks terminate with no dispatch under
`TRAP(ON,NOSPIE)`).

`stdc_raise` needs no z/OS changes beyond `prepare_rsi`/`invoke_sigaction`
and the setjmp defines: the frame walk (`sigismember(frame->guarded, signo)`,
decider switch, `WG14_SIGNALS_LONGJMP(frame->buf, 1)`) and the global-decider
chain are copied verbatim.

To avoid the POSIX/zOS copies drifting, leave a comment in both files
pointing at each other (the Windows backend already establishes this
convention). A future refactor could move `sigguarded`/`stdc_raise` into the
common include guarded by a `WG14_SIGNALS_HAVE_POSIX_STYLE_RAISE`
macro; do that only if a fifth backend appears.

### `install_sighandler_impl` / `uninstall_sighandler_impl`

```c
  static bool WG14_SIGNALS_PREFIX(install_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = WG14_SIGNALS_PREFIX(raw_signal_handler);
    sa.sa_flags = SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER;
    // Empty mask: like the POSIX backend we rely on SA_NODEFER and manage
    // blocking ourselves; the OMR z/OS port uses exactly
    // sigemptyset(sa_mask) + SA_SIGINFO|SA_NODEFER|SA_RESTART. SA_RESTART is
    // NOT set here (matching the POSIX backend's flags; document that
    // interrupted syscalls get EINTR, and that z/OS's SA_RESTART list is
    // documented in the Runtime Library Reference if it is ever wanted).
    (void) sigemptyset(&sa.sa_mask);
    if(-1 == sigaction(signo, &sa, &item->old_handler))
    {
      return false;
    }
    return true;
  }

  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    (void) sigaction(signo, &item->old_handler, WG14_SIGNALS_NULLPTR);
    return true;
  }
```

### Skip the uncatchable z/OS signals in the install loops (common .ipp change)

`siginstall(NULL)` (and `signal_decider_create`) iterate `1..NSIG-1` and
install every member of the (filled) set. On z/OS the following cannot be
caught or ignored, and `sigaction()` on them fails: SIGDUMP, SIGTHSTOP,
SIGTHCONT, SIGTRACE (plus the unconditionally-delivered SIGKILL/SIGSTOP
already skipped). Without a skip, a default `siginstall(NULL)` would fail at
the first uncatchable and roll back.

The common install/uninstall/decider loops each begin
`if(signo == SIGKILL || signo == SIGSTOP) continue;` — at 6 sites:
`thrd_signal_handle_common.ipp.ipp` lines 522, 550, 584, 623, 662 and 735.
The repo already has a platform-specific inline skip at the first two of
these sites with `#ifdef __FILC__` calling `zis_unsafe_signal_for_handlers()`
(lines 526-531 and 554-559); follow that exact convention and add the z/OS
skip to all six sites:

```c
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
#ifdef __MVS__
      // z/OS: signals that cannot be caught or ignored (LE Programming
      // Guide, "POSIX signals that do not enter condition handling").
      if(signo == SIGDUMP || signo == SIGTHSTOP || signo == SIGTHCONT ||
         signo == SIGTRACE)
      {
        continue;
      }
#endif
#ifdef __FILC__
      if(zis_unsafe_signal_for_handlers(signo))
      {
        continue;
      }
#endif
```

(The `__FILC__` blocks appear only in the install/rollback loops today;
mirror the addition at all six sites for z/OS. `SIGCONT`/`SIGTSTP`/`SIGTTIN`/
`SIGTTOU` *can* be caught on z/OS even though they bypass LE condition
handling when their default action applies — installing handlers for them is
legal and useful, so they are not skipped.)

### `current_thread_id.c.ipp` (see §"Repository integration", item 5)

### Phase 2: LE exception-handler backstop for `sigguarded()` (a6eh)

The design, rationale, code, registration point and open points are in
§Design → Phase 2 (the "program check → software-signal bridge"). Reference
the same file sections when implementing; the runtime-option detection below
is diagnostic-only (it must NOT gate the bridge).

Runtime option detection (diagnostic; also used by a Phase 1 startup probe in
debug builds):

```c
// The CEEOCB is not a public C API surface; the option indexes below are
// taken from the OMR port's omrceeocb.h mapping of the documented LE
// CEEOCB layout (the runtime options in CEEOCB order: __posix is the 22nd,
// __trap the 33rd option entry in that header's enum). They are
// best-effort diagnostics only, never a correctness dependency, and the
// probe (Appendix A.2) verifies them empirically against the runtime
// options it prints for the same run.
#define WG14_SIGNALS_ZOS_OPT_INDEX_POSIX 22
#define WG14_SIGNALS_ZOS_OPT_INDEX_TRAP 33
  static int WG14_SIGNALS_PREFIX(zos_ceeocb_opt_on)(const int opt_index)
  {
    // ceeocb() == ((struct ceeocb *)(ceeedb()->ceeedboptcb)) where
    // ceecaa() == ((struct ceecaa *)__gtca()) and
    // ceeedb() == ((struct ceeedb *)ceecaa()->ceecaaedb); the option array
    // entry is `ceeocb_opt[opt_index].ceeocb_opt_on` (a 1-bit field).
    ...  // see OMR port/zos390/omrceeocb.h for the exact field packing
  }
  static int WG14_SIGNALS_PREFIX(zos_posix_mode)(void)
  {
    return WG14_SIGNALS_PREFIX(zos_ceeocb_opt_on)(WG14_SIGNALS_ZOS_OPT_INDEX_POSIX);
  }
  static int WG14_SIGNALS_PREFIX(zos_trap_spie)(void)
  {
    // TRAP on/off from option 33, then the sub-options block's
    // ceeocb_trap_spie bit (at ceeocb_opt_subopts_offset).
    ...
  }
```

Because the CEEOCB layout is not a public C API surface, the probe
(Appendix A.2) prints the observed modes; the library's own detection is
best-effort diagnostics only, never a correctness dependency.

## Test plan

No z/OS machine in CI. Verification splits into (a) what can be checked in
this repo, and (b) the machine-side validation program in Appendix A.

1. **Off-platform compile checks** (best effort on macOS/Linux): a `zos`
   compile-only leg is only possible with a z/OS toolchain, which CI lacks;
   instead:
   - keep the z/OS backend's `#error` guard, so a wrong-platform include
     fails loudly (ideas.md 4.4 pattern);
   - the non-z/OS-touching parts of the change set (config.h TLS default,
     current_thread_id `__MVS__` branch, the six `#ifdef __MVS__` skips) are
     compiled on every CI platform and must be behavioural no-ops there;
   - the z/OS-specific code itself is validated by code review and the
     machine-side probe (Appendix A).
2. **Behavioural equivalence**: because the z/OS backend is the POSIX
   backend with a thin adaptation layer, the entire existing test suite
   (`thrd_signal_handle_test`, `decider_*_test`, `sigguarded_tss_init_test`,
   `stdc_raise_*_test`, `post_uninstall_reentry_test`, header-only C/C++
   consumers, install consumer) is the regression suite for the shared
   logic; on z/OS the following tests are expected to pass with no changes:
   `thrd_signal_handle_test` (SIGUSR1), `decider_mixed_set_test`,
   `siguninstall_raise_test`, `stdc_raise_zero_test`,
   `stdc_raise_null_info_test`, `stdc_raise_uninstalled_test`,
   `sigguarded_tss_init_test`, `install_sighandler_lock_test`,
   `signo_map_verstable_init_test`, `decider_orphan_reinstall_test`,
   `post_uninstall_reentry_test`, `standalone_setup_test`,
   `async_signal_safe_tls_test`, `tss_concurrent_exit_test`,
   `header_only_test`, `header_only_c_multi_test`.
3. **Exclusions on z/OS** (record in `test/CMakeLists.txt`):
   - `thrd_sigfpe_test` and `recovery_null_loop_test` use *guarded
     recovery* (siglongjmp out of the catcher, never a catcher return), so
     they are compatible with the z/OS RETURN-terminates rule and are
     expected to *pass* under `POSIX(ON)` + `TRAP(ON,SPIE)` (the default);
     keep them enabled, but note in the CMake comment that they need
     `TRAP(ON,SPIE)` (they fail under `TRAP(ON,NOSPIE)`, where no program
     check reaches the catchers) and confirm via probe A.5 first.
   - `siginstall_rollback_test`: needs GNU/BSD `--wrap` (z/OS ld has no
     equivalent); exclude like MSVC/Apple/Fil-C.
   - `sigfence_codegen_test`: inspects x86 assembly; exclude.
   - `benchmark_*`: same as other platforms (CI `-E benchmark`).
4. **New z/OS-specific tests** (to be written for the machine-side probe,
   not for CI): signal-set contents; `siginstall(NULL)` over the full
   NSIG=42 range (exercises the uncatchable-skip hook); stdc_raise of a
   software signal with/without guards; a program-check SIGSEGV/SIGFPE
   guarded recovery; decider resume_execution on a software raise; the
   previously-installed-handler chain for SIG_DFL/SIG_IGN/handler cases,
   including the ZOS6 split (real fault terminates; software raise of the
   same signal is still ignorable); Phase 2 bridge re-raise reaching the
   catcher exactly once (probe A.6).

## Validation

Per AGENTS.md rule 3 the CI recipe is `.github/workflows/ci.yml`; there is no
z/OS leg, so validation is:

1. **Local**: build and run the full suite on Linux/macOS (Debug+Release,
   C11+C23, shared ON/OFF, fallback TLS ON/OFF) to prove the shared-logic
   diffs (the six `#ifdef __MVS__` signal skips, config.h, current_thread_id)
   are no-ops off z/OS. The changes to `thrd_signal_handle_common.ipp.ipp`,
   `config.h` and `current_thread_id.c.ipp` must not perturb any existing
   platform: this is the strongest verification available here.
2. **clang-format** on every changed header/source (AGENTS.md rule 2);
   C11 only under `include/` and `src/`; C++ only in `test/`.
3. **Machine-side**: run Appendix A's probe on a real z/OS 3.x system
   (AMODE 64, POSIX(ON)), then the full test suite minus the exclusions in
   §"Test plan". Record results in this plan / analysis.md.

## Documentation updates

Per AGENTS.md rule 6 and the repo's plan convention:

- `plans/analysis.md`: add a new findings row (new platform section, not a
  defect): `ZOS1` — z/OS has no `si_addr`; `si_code` substitutes.
  `ZOS2` — RETURN from a program-check-driven SIGABND/SIGFPE/SIGILL/SIGSEGV
  catcher terminates the process; `resume_execution` unimplementable for
  program checks. `ZOS3` — LE exception handlers are frame-scoped and
  cannot return; a signals-only backend is the only complete design.
  `ZOS4` — uncatchable z/OS signals (SIGDUMP/SIGTHSTOP/SIGTHCONT/SIGTRACE)
  must be skipped by the install loops. `ZOS5` — z/OS requires
  `POSIX(ON)` + `TRAP(ON,SPIE)` (default) for the signal path and
  `TRAP(ON,NOSPIE)` for exception handlers; the library documents rather
  than changes runtime options. `ZOS6` — replacing a SIG_IGN disposition of
  SIGABND/SIGFPE/SIGILL/SIGSEGV with a catcher makes real program
  checks/ABENDs ignorable no longer (catcher return terminates); documented
  on `siginstall()` for z/OS.
- `plans/ideas.md`: add the `__MVS__` platform-branch technique and the
  "re-raise to emulate SIG_DFL/SIG_IGN from a catcher" technique under §2.
- `Readme.md`: add z/OS (AMODE 64, POSIX(ON)) to the supported-platform
  matrix with the two documented deviations (resume_execution for program
  checks; no si_addr).
- `include/wg14_signals/thrd_signal_handle.h`: document the z/OS
  deviations on `stdc_raise`/`sigguarded`/`sigfillset_*` doc comments.
- `docs/proposed-wording.md` is vendored WG14 wording — no changes unless
  the deviations need a footnote (record as a recommendation, not a
  change).

## Risks / open decisions

1. **Catcher-return semantics for program checks (fact 4) are the largest
   risk.** The docs are unambiguous ("the process will be terminated") for
   SIGABND/SIGFPE/SIGILL/SIGSEGV under POSIX(ON), but do not state what
   happens for other signals, nor whether `TRAP(ON,SPIE)`'s documented
   resumability changes this for sigaction catchers (the LE guide's C-model
   example shows return-and-resume-at-next-instruction under POSIX(OFF)).
   Probe A.5 decides whether `resume_execution` can be supported at all and
   whether the SIG_IGN/SIG_DFL re-raise emulation in `invoke_sigaction` is
   needed or whether plain return suffices on TRAP(ON,SPIE) machines.
2. **CIB/MCH field names.** `cib_pcf`, `cib_abf`, `cib_machine`,
   `__mch_bea` are confirmed by OMR's production code, but the C header
   names must be verified against the installed `<__le_cib.h>`/`<__le_mch.h>`
   (Appendix A.4). If they differ, the mapping code changes are mechanical.
3. **`__reset_exception_handler()` from inside the handler — RESOLVED.** The
   `__reset_exception_handler()` documentation ("for the current stack
   frame", EINVAL otherwise) settles it: a running handler cannot unregister
   the guard frame. Phase 2's design therefore needs no unregistering: the
   bridge re-raises the mapped signo as a software signal and terminates
   via `__cabend()` only if control returns. Probe A.6(b) confirms the
   re-raise reaches the catcher exactly once, without recursion.
4. **`sigsetjmp` macro-vs-function**: XL C's `setjmp` is a macro; ensure the
   `WG14_SIGNALS_SETJMP(buf) sigsetjmp((buf), 1)` define is not clobbered by
   header ordering (define after all includes, before use, as in the
   skeleton).
5. **z/OS `siginfo_t` tag**: the plan aliases `siginfo_t` directly (like the
   `__FILC__` branch), removing all dependence on the struct tag spelling;
   probe A.3 confirms the field order.
6. **jmp_buf sizing/XPLINK** (fact 13): document the constraint; the library
   must be built with the same LE headers/linkage as the application.
7. **TRAP(ON,SPIE) vs NOSPIE**: Phase 1 requires only the default
   TRAP(ON,SPIE); if the application runs TRAP(ON,NOSPIE), program checks
   never reach the sigaction catchers (no signals are generated) — Phase 1
   detects this in a debug probe and warns; Phase 2's exception-handler
   bridge (design §Phase 2) is the remedy: it re-raises guarded program
   checks as software signals, which work under both TRAP modes.
8. **`SA_RESTART` omitted**: matches the POSIX backend; document the EINTR
   behaviour on z/OS.
9. **Phase 2 scope**: deliberately optional; the only remaining Phase 2
   uncertainty is the CIB→signo field mapping (Appendix A.4) — if that
   cannot be resolved on the machine, Phase 2 is dropped and Phase 1's
   "warn under TRAP(ON,NOSPIE)" stays as the documented limitation.
10. **XL C toolchain gaps**: (a) the common code requires C11
    `<stdatomic.h>` — gate the z/OS build on the toolchain providing it
    (z/OS Clang does; older XL C may not); (b) GNU `__attribute__((constructor))`
    on the sigset functions is unconfirmed on XL C — guard it (see
    §"sigfillset tables") and substitute the benign lazy-init race;
    (c) XL C has no GCC-style warning flags — the CMake branch uses
    `-qflag=w:w -qhalt=s` so odd prologue diagnostics become readable files,
    not fatal errors.
11. **z/OS feature-test macro exclusivity** (risk if the generic CMake FTM
    ladder is left active on z/OS): `_XOPEN_SOURCE=700`/`_GNU_SOURCE` can
    contradict z/OS's `_XOPEN_SOURCE_EXTENDED`, and `_OPEN_THREADS` vs
    `_UNIX03_THREADS`/`_XOPEN_SOURCE 600` are mutually exclusive. The CMake
    plan therefore bypasses the ladder on z/OS with the explicit three-macro
    set; if a future CMake refactor touches lines 129-164, re-check the
    `zos` bypass.
12. **`siginfo_t` context argument type**: the runtime reference calls the
    sa_sigaction third argument a `ucontext_t`; OMR reinterprets it as the
    machine context `__mcontext_t_` (z/OS header `edcwccwi.h`), which
    overlays it. The library only passes the pointer through, so the
    ambiguity is safe, but the probe (A.5) should print both interpretations
    to document which one the runtime actually supplies.
13. **Real-fault *ignore* replacement (ZOS6).** Replacing a SIG_IGN
    disposition for SIGABND/SIGFPE/SIGILL/SIGSEGV changes effective
    behaviour for real program checks/ABENDs (they terminate on catcher
    return; software raises of the same signals remain ignorable). This is
    inherent to z/OS POSIX(ON), not a library bug, but must be documented on
    `siginstall()`; probe A.8 confirms the split, and the guard-recovery
    path remains the recommended way to handle real faults.
14. **Phase 2 bridge and `__set_exception_handler` return value**: `Hook 1`
    treats failure as non-fatal, but if registration *succeeds* under
    `TRAP(ON,SPIE)` while the handler is never driven, nothing changes; if it
    succeeds and the handler IS driven, the bridge re-raise requires the
    sigaction catcher to be installed for the mapped signo (i.e. guarded
    faults still need `siginstall()` or a decider that dispatches via the
    global chain — matching the POSIX contract). Probe A.6 verifies the
    driven/never-driven behaviour per TRAP mode.

## Appendix A — machine-side probe program (to run on real z/OS)

`test/zos_probe.c` (not wired into CI; a standalone `add_code_example`
target or a script in `prompts/`). Prints diagnostics for:

1. Compiler/runtime identity: `__MVS__`, `_LP64`, `__XPLINK__`,
   `__STDC_VERSION__`, LE version; whether the toolchain provides C11
   `<stdatomic.h>` and accepts `__attribute__((constructor))` and
   `__attribute__((tls_model("initial-exec")))`.
2. Runtime options: POSIX(ON)? TRAP mode? via the CEEOCB walk (prints raw
   bytes as well, for offset verification).
3. siginfo_t/sigaction/ucontext compile-time facts: `sizeof(sigset_t)`,
   `NSIG`, `sizeof(siginfo_t)`, field order (si_signo/si_errno/si_code/
   si_pid/si_uid), the sa_sigaction third-argument type it compiles against
   (`ucontext_t` vs `__mcontext_t_`), `sigemptyset/sigfillset` round-trip.
4. CIB/MCH: inside a same-thread program-check catcher (`*(volatile int *)0`
   or an IEEE divide-by-zero under TRAP(ON,SPIE)), print
   `__le_cib_get()`, `cib->cib_pcf/cib_abf`, `cib->cib_machine`,
   `__mch_bea` vs the `ucontext_t` PSW.
5. Return semantics: install a `SA_SIGINFO` catcher for SIGSEGV, fault, and
   return; does the process terminate, resume at next instruction, or
   re-execute? Repeat for a software `raise(SIGSEGV)` and for SIGBUS/SIGSYS.
6. Phase 2 mechanics: `__set_exception_handler` from a known frame; inside
   the handler, (a) confirm `__reset_exception_handler()` fails with EINVAL
   (documented expectation, now confirmed by the docs); (b) verify the
   unregister-free fallback: `pthread_kill(pthread_self(), SIGSEGV)` reaches
   the installed sigaction catcher exactly once (no re-entry into the
   exception handler, no infinite loop) and that returning from the catcher
   resumes the suspended handler, which then terminates via `__cabend()`.
   Also test the SIG_DFL/SIG_IGN re-raise emulation from a normal catcher
   (no infinite loop).
7. `siginstall(NULL)` over all of 1..NSIG-1 with the uncatchable skip —
   confirm only the four skipped signals are rejected by sigaction.
8. `ZOS6` confirmation: set `SIG_IGN` on SIGSEGV, then replace it with a
   `SA_SIGINFO` catcher that re-establishes SIG_IGN and re-raises (the
   `invoke_sigaction` emulation), then trigger a *real* program check —
   confirm the process terminates (documented rule) rather than resuming.
   Repeat with `raise(SIGSEGV)` (software) — confirm the software raise is
   discarded and execution continues. This pins down whether the
   re-raise-and-discard emulation achieves ignore for software signals only.

## Appendix B — condition → signal mapping (from the a6eh docs and LE guide)

| Condition (feedback code) | Signal | Notes |
|---|---|---|
| CEE341–CEE343 (operation/privileged-op/execute), CEE346 (specification) | SIGILL | |
| CEE344–CEE345 (protection/addressing) | SIGSEGV | |
| CEE347 (data), CEE349 (fixed divide), CEE34A–CEE34C (decimal/exponent), CEE34E–CEE34P, CEE352, CEE354–CEE358 (IEEE/vector) | SIGFPE | |
| CEE348 (fixed overflow), CEE34D (exp underflow), CEE34E (significance) | n/a | "not processed in a C/C++ application" (masked) |
| CEE35I (retryable abend) | SIGABND | |
| S/370 interrupt codes 01–03, 06 → SIGILL; 04, 05 → SIGSEGV; 07, 09–12, 15 → SIGFPE; 08, 13, 14 → n/a (masked) | | LE guide Table 16 |
| Abends (SVC 13 user-initiated, MVS-initiated) | SIGABND | LE guide Table 17 |

## Appendix C — z/OS signal numbering (from the Go z/OS port, `zerrors_zos_s390x.go`, matching Table 57 and `NSIG=42`)

SIGHUP 1, SIGINT 2, SIGABRT 3, SIGILL 4, SIGPOLL 5, SIGURG 6, SIGSTOP 7,
SIGFPE 8, SIGKILL 9, SIGBUS 10, SIGSEGV 11, SIGSYS 12, SIGPIPE 13,
SIGALRM 14, SIGTERM 15, SIGUSR1 16, SIGUSR2 17, SIGABND 18, SIGCONT 19,
SIGCHLD 20, SIGTTIN 21, SIGTTOU 22, SIGIO 23, SIGQUIT 24, SIGTSTP 25,
SIGTRAP 26, SIGIOERR 27, SIGWINCH 28, SIGXCPU 29, SIGXFSZ 30, SIGVTALRM 31,
SIGPROF 32, SIGDANGER 33, SIGTHSTOP 34, SIGTHCONT 35, SIGTRACE 37,
SIGDCE 38, SIGDUMP 39 (36, 40, 41 not in the Go port's list; NSIG=42).

Uncatchable/always-take-effect: SIGKILL, SIGSTOP, SIGDUMP, SIGTHSTOP,
SIGTHCONT, SIGTRACE. Do not enter LE condition handling when defaulted:
SIGKILL, SIGSTOP, SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU (+ the four
uncatchables).
