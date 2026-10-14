# Extend glibc with the wg14_signals threadsafe signals implementation

## Goal

Extend the vendored `glibc/` checkout in this workspace (the `ned14/glibc`
fork, branch `master`, head `0d5632ba0e` "Have github CI run the test suite.",
remote `git@github.com:ned14/glibc.git` — verified 2026-08-31) with the WG14
N3924 improved-C-signals API, in exactly the same way the sibling
`llvm-project/` fork's `libc/` subproject was extended (see
`plans/llvm-project-fork.md`, which is the template this plan is derived
from; §"The llvm fork's git history" below records what the fork's commit
history adds to that template — this plan is the second-generation
integration and incorporates every lesson it yields). The reference
implementation is this repo's `wg14_signals` library
(`include/wg14_signals/` + `src/wg14_signals/`), consumed **unmodified and
zero-drift** as a git submodule of the glibc fork. The extension must:

1. Keep every existing ISO-C/POSIX signal entrypoint in glibc
   (`raise`, `signal`, `sigaction`, `sigprocmask`, `pthread_sigmask`,
   `sigaltstack`, `kill`, `killpg`, `sigqueue`, `sigwait*`, the sigset
   helpers, `abort`, `system`) with conformant behaviour — a glibc user must
   observe **no regression** in the existing API surface.
2. Expose the full N3924 API as new glibc entrypoints in `libc.so.6` /
   `libc.a`, declared in glibc's installed `<signal.h>` (clause 7.14.2.x /
   7.14.3.x / 7.14.4 of the proposal wording) and `<threads.h>`
   (7.30.6.5–8): `sigguarded`, `sighandle`, `siginstall`, `siguninstall`,
   `siguninstall_system`, `signal_decider_create`, `signal_decider_destroy`,
   `sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`,
   `sigfillset_asynchronous_debug`, `sigdecider_abandon`,
   `sigdecider_abandon_resume`, `siginstall_set_sa_flags_np`,
   `siginstall_set_default_action_np`, the `sigfence` macro,
   `SIGGUARDED_FAILURE_VALUE`, the `stdc_siginfo` / `stdc_siginfo_value` /
   `sig_decision` / `sig_func_t` / `sig_recover_t` / `sig_decide_t` types,
   `tss_async_signal_safe_create` / `tss_async_signal_safe_destroy` /
   `tss_async_signal_safe_thread_init` / `tss_async_signal_safe_get` with
   `tss_async_signal_safe_t` / `tss_async_signal_safe_attr`, and
    `current_thread_id` with `thread_id_t` (reference-impl extension, exposed
    in `<threads.h>` like the llvm-libc fork did). `stdc_raise` (7.14.2.10)
    is also declared in `<signal.h>` and, with the submodule pin advanced to
    `2217d72`, is now exported by libc too: the reference implementation
    defines it (POSIX real self-delivery, with the caller's
    `raw_info`/`raw_context` carried to the raw handler by a stack-allocated
    record linked into the calling thread's existing per-thread state, a
    stack-aware token and a per-thread sequence; Windows a real
    `RaiseException()`), so glibc mirrors both the declaration and the
    exported definition.
3. Work on Linux across all glibc architectures, with a documented gating
   policy for Hurd and a deferred assessment for the rest.

Integration decisions (2026-08-31, agreed with the user):

- **Submodule**: the reference implementation is mounted as a git submodule
  at `glibc/wg14_signals/` (URL `https://github.com/ned14/wg14_signals.git`),
  pristine and zero-drift, **pinned to
  `2217d72eb0d1266eb8e97f2b5df0e83792b73867` (`2217d72` "Implement stdc_raise()
  with the raw_info/raw_context contract")** — the current upstream `main`
  HEAD and exactly the pin the llvm fork settled on (Appendix A) — mirroring
  the llvm-libc fork's `libc/src/signal/wg14` submodule. All glibc-owned
  integration files live in a **new top-level glibc build subdir
  `glibc/wg14/`** (git cannot track files inside a submodule directory — the
  lesson learned in the llvm-libc fork, where the fork-owned files had to move
  to the sibling `libc/src/signal/wg14-build/`).
- **Public API placement**: the N3924 API is declared in glibc's own
  installed `<signal.h>` (`signal/signal.h`) and `<threads.h>`
  (`sysdeps/pthread/threads.h`), gated behind `__USE_GNU` (glibc convention
  for non-standard extensions), with the declarations **mirrored verbatim**
  from the reference headers and kept in sync by a diff test.
- **Existing entrypoints untouched**: glibc's `raise`/`sigaction`/
  `sigprocmask`/`abort`/`system`/`posix_spawn`/nptl keep their current
  implementations; they never call the wg14 API surface (same policy as the
  llvm-libc fork: internal libc never calls `siginstall`/`siguninstall`/
  `signal_decider_*`/`sighandle`; the last writer of a signal's kernel
  disposition wins; external `siginstall` coexists per-signal).

Everything below is derived from direct study of both trees at the current
checkouts: `glibc/` at `0d5632ba0e` (remote `git@github.com:ned14/glibc.git`)
and this repo at HEAD. Paths are relative to this workspace root unless
stated otherwise.

## Implementation status (2026-08-31, implementer's record)

All fork-side work tracked below has been implemented **in the working
tree** (nothing committed — AGENTS.md rule 10): the submodule is mounted
at `glibc/wg14_signals/` pinned to `2217d72` (`.gitmodules` staged); the
`wg14` build subdir (`Makefile`, `embedder_shim.h`, `Depend`, `Versions`,
15 ported tests, `check-wg14-mirror.py`); `Makeconfig` all-subdirs; the
`signal/signal.h` + `sysdeps/pthread/threads.h` mirror blocks; the
`GLIBC_2.44` abilist lines for all 32 Linux arches; the fork CI's
`submodules: recursive` checkout; the NEWS entry and this repo's
Readme.md.  Verification available on this (macOS) host: the mirror sync
check runs clean against the submodule; every test and every wg14 libc
object TU passes `-fsyntax-only` against a synthesised glibc header
harness (mirror blocks over host headers) in all eight
check-installed-headers modes (c89/gnu89/c11/gnu11 x c++98/gnu++98/
c++11/gnu++11).  What still needs a real Linux build/configures: the
Phase 1 build itself, `make check` (incl. `check-installed-headers`,
`conform`, `check-abi`), and `make update-all-abi-libc` to confirm the
hand-added abilist entries.

**Deviations from the plan discovered during implementation** (all
incorporated above; the plan text below stands as the record):

1. **Same-TU tag redefinition is NOT valid C** (plan §"Conflicts to
   expect and defuse"): GCC and Clang reject repeated identical
   struct/union/enum tag definitions in one translation unit even when
   compatible (only typedef/declaration repetition is legal), so the
   wg14 libc TUs cannot see both the mirror and the reference header's
   `union stdc_siginfo_value`/`struct stdc_siginfo`/`enum sig_decision`.
   The mirror therefore gates its whole declarations region (types
   through function declarations) behind
   `#if !defined WG14_SIGNALS_GLIBC_EMBEDDED`, a marker defined ONLY by
   `wg14/embedder_shim.h`; consumer translation units never define it.
   (The llvm-libc fork never hit this because `WG14_SIGNALS_PREFIX`
   namespaces the submodule's types away from the libc mirrors.)
2. **The four handler typedefs are FUNCTION types, not pointers**:
   `typedef union stdc_siginfo_value (sig_func_t) (...)` etc. — a
   `(*sig_func_t)` mirror spelling normalises identically in the sync
   checker but is a different C type and a same-TU redefinition error;
   the checker now bans the pointer spellings outright.
3. **Hurd gating spelling is `gnu-gnu`, not `gnu`** — configure.ac
   mutates the Hurd `host_os` before `config.make` is generated; the
   wg14 Makefile gates on `ifeq ($(config-os),gnu-gnu)`.
4. **The abilist data-symbol type is `T`, not `D`**: `scripts/abilist.awk`
   classifies `.tbss`/`.tdata` symbols as `T` (glibc's own
   `__libc_single_threaded D 0x1` is plain data, not TLS).  Hand-added
   `GLIBC_2.44 current_thread_id_cached T 0x8|0x4` per arch; the first
   Linux build's `make update-all-abi-libc` is the authoritative check.
5. **Sigfence machinery adjustments** required by
   check-installed-headers' strict modes: the argument-count
   `_Static_assert` is mode-guarded (c89/gnu89/c++98 skip it), the
   union's `constexpr` ctors and the C++ `constexpr`
   SIGGUARDED_FAILURE_VALUE are `__cplusplus >= 201103L`-guarded (C++98
   gets a brace-init fallback), and the volatile-sink is renamed
   `__wg14_sigfence_sink`.
6. **The mirror sync checker** (`wg14/check-wg14-mirror.py`,
   tests-special) implements the plan's normalisation list: comments and
   `#`-lines stripped, `WG14_SIGNALS_PREFIX`/`EXTERN`/`INLINE`/
   `C23_CONSTEXPR_OR_CONST`/`STATIC_ASSERT` un-macroed, `(`,`)`,`*`,`{`,
   `}` token-dropped (glibc's function-typedef and braced spellings then
   match the reference's macro-derived line splits), the TLS idiom mapped
   (`__attribute__((tls_model("initial-exec")))` dropped,
   `ASYNC_SAFE_THREAD_LOCAL`→`__thread`), the sink name canonicalised,
   plus the FORK_DROPS list for the non-glibc platform-ladder forks and
   the pointer-typedef ban (deviation 2).  It passes on both mirrors and
   was regression-tested against both deviation 2 spellings.
7. **14 tests ported** (`glibc/wg14/tst-wg14-*`, the plan's list minus
   the two optional items), glibc-native (support/check.h,
   fork+waitpid/waitid death arms, atomics for synchronisation, no
   sleeps-alone); tst-wg14-chain's `sigfence()` arm exercises the
   mirrored macro in-tree; tst-wg14-sa-flags-np adds the alt-stack and
   SIGCHLD-reap arms (a real kill(), not sighandle, so the alt stack is
   actually used); tst-wg14-default-action REQUIRES the
   siginfo-preserving `SYS_rt_tgsigqueueinfo` handoff and verifies
   `si_addr` fidelity through a genuine SIGSEGV; tst-wg14-abort covers
   the abort-lock interplay.
8. **clang-format** was run on every fork-owned new file (shim, tests);
   the mirror blocks inside `signal/signal.h`/`threads.h` are left
   unformatted by design — glibc's `.clang-format` reflows the sigfence
   machinery and the declaration line structure, which the verbatim
   mirror and the sync check require (and glibc never formats its
   pre-existing headers wholesale).

## Ordered implementation task list (for the implementing agent)

This is the executive summary; each item is expanded in the sections below.

1. Add wg14_signals as a git submodule of the glibc fork at
   `wg14_signals/`, pinned to `2217d72eb0d1266eb8e97f2b5df0e83792b73867`
   (§"Submodule integration"). Requires the glibc fork's CI to checkout with
   `submodules: recursive` (Phase 4).
2. Create the glibc-owned build subdir `glibc/wg14/` (§"Build wiring"):
   `Makefile` (routines + tests + Depend), the force-included
   `embedder_shim.h`, the `Versions` file, and `glibc/wg14/tst-*.c` tests.
   Add `wg14` to `all-subdirs` in `Makeconfig:1441`. The subdir compiles
   the submodule's four POSIX sources (`src/wg14_signals/{current_thread_id,
   tss_async_signal_safe,thrd_signal_handle_posix,thread_atexit}.c`) with
   `-I $(..)wg14_signals/include`, a `vpath %.c` to the submodule's source
   dir, and the force-included shim defining the embedder override macros
   (§"Embedder override layer"). Objects default to `MODULE_NAME=libc`
   (`Makeconfig:1100-1110`) and therefore get glibc's
   `-ftls-model=initial-exec` (`Makeconfig:1105-1107`) — exactly the TLS
   model the reference implementation requires.
3. Extend the installed public headers (§"Public API surface"):
   `signal/signal.h` gains the N3924 signal API under `__USE_GNU`;
   `sysdeps/pthread/threads.h` gains the four `tss_async_signal_safe_*`
   functions plus `current_thread_id`/`thread_id_t` (per N3924 rev-5
   wording 7.30.6.5–8, `docs/proposed-wording.md`; `current_thread_id` is
   the reference implementation's own API, not in the wording — llvm-libc
   exposed it in `<threads.h>` too). Mirrors must be layout-identical to
   the reference headers (a sync test in Phase 3 enforces this).
4. Add the symbol versions and ABI entries (§"Symbol versioning and ABI"):
   `glibc/wg14/Versions` with a `GLIBC_2.44` node for the 19 new public
   functions + the `current_thread_id_cached` TLS data symbol, and a
   `GLIBC_PRIVATE` entry for the internal `thread_atexit` helper; add the
   `GLIBC_2.44` lines to **every** `sysdeps/unix/sysv/linux/*/libc.abilist`
   (mechanically via `make update-all-abi-libc` after the build).
5. Verify with a smoke test that exercises the N3924 API against the built
   libc on Linux (§"Phase 1"), then port the meaningful wg14_signals tests
   as glibc tests in `glibc/wg14/` and run `make check` (§"Phase 3").
6. Update the fork CI (`.github/workflows/ci.yml`): `submodules: recursive`
   checkout; keep `configure && make && make check` (§"Phase 4").
7. Documentation: glibc `NEWS` entry, manual node (optional), and this
   repo's `Readme.md` supported-targets table (§"Phase 5").
8. Hurd assessment and gating (§"Phase 6"): the reference's default
   `WG14_SIGNALS_GETTID` (`syscall(SYS_gettid)`) does not compile on Hurd
   (no `syscall()`), so Hurd needs either a gated-out wg14 subdir or a
   Hurd-specific shim — defer, but the subdir must not break Hurd builds
   from day one.

## Background: what exists in each tree today

### glibc signal handling today

- **Public signal API**: `signal/` subdir (`signal/Makefile`) implements the
  ISO-C/POSIX surface. The kernel-facing entrypoints:
  - `signal/sigaction.c` — `__sigaction` validates `0 < sig < NSIG` and
    **rejects glibc's internal signals** (`is_internal_signal`, EINVAL —
    SIGCANCEL = `__SIGRTMIN` = 32, SIGSETXID = 33,
    `sysdeps/unix/sysv/linux/internal-signals.h`), takes the abort rwlock
    (the **write** lock, for queries and sets alike) when `sig == SIGABRT`
    (serialises with `abort()`; §"Comprehensive review" item 1 analyses the
    signal-handler-context hazard this creates for the shim), then calls
    `__libc_sigaction`; `sigaction` is a weak alias.
  - `sysdeps/unix/sysv/linux/libc_sigaction.c` — `__libc_sigaction`, the
    raw `rt_sigaction` syscall wrapper (kernel `struct kernel_sigaction`
    conversion, `__NSIG_BYTES`, `SA_RESTORER` handling per arch).
  - `sysdeps/posix/raise.c` — `raise(sig)` = `__pthread_kill(__pthread_self(),
    sig)` (self-directed tgkill).
  - `nptl/pthread_kill.c` — `pthread_kill` (`__pthread_kill_internal` →
    tgkill; EINVAL for internal signals; versioned `GLIBC_2.34`).
  - `sysdeps/posix/system.c` — `system()` does the POSIX SIGINT/SIGQUIT
    save-ignore-restore via `__sigaction` (with a reference-counted lock),
    then `posix_spawn` with `POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK`.
  - `stdlib/abort.c` — `raise(SIGABRT)` first; **only if that returns**
    (handler/decider returned, or SIGABRT blocked/ignored) it takes the
    abort lock, resets SIGABRT to `SIG_DFL`, re-raises, unblocks, and falls
    through to `ABORT_INSTRUCTION` + `_exit(127)`. This
    "reset-to-SIG_DFL before re-raise" property is exactly what the
    wg14_signals `WG14_SIGNALS_ABORT` hook contract requires (no infinite
    loop).
  - Sigset helpers (`sigempty.c`, `sigfillset.c`, `sigaddset.c`,
    `sigdelset.c`, `sigismem.c`, `sigsetops.c`) are public entrypoints
    already — the reference implementation's sigset hooks resolve
    naturally to them (contrast with llvm-libc, which had to *add*
    `sigismember`).
- **Internal signal consumers**: nptl's SIGCANCEL/SIGSETXID (rejected by
  the public sigaction — a feature for us, see §"Comprehensive review"),
  timer_create's SIGTIMER (= SIGCANCEL), ld.so lazy-binding (no signals),
  `__libc_current_sigrtmin` for dynamic SIGRTMIN.
- **Headers**: installed `<signal.h>` = `signal/signal.h` (feature-test
  gated; `ucontext_t` under `__USE_XOPEN_EXTENDED || __USE_XOPEN2K8`;
  `siginfo_t` under `__USE_POSIX199309`); installed `<threads.h>` =
  `sysdeps/pthread/threads.h` (installed via `sysdeps/pthread/Makefile`
  `headers += threads.h` under the `nptl` subdir); internal overrides
  `include/signal.h`, `include/threads.h`.
- **ABI**: symbol versions in per-subdir `Versions` files (e.g.
  `signal/Versions`); exported-symbol check via
  `sysdeps/unix/sysv/linux/<arch>/libc.abilist` (`check-abi` compares the
  built libc's dynsym against it, `Makerules:1329-1339`; `make
  update-all-abi-libc` patches all archs, `Makerules:1357-1364`). The tree
  is at version 2.44.9000 (development); `GLIBC_2.44` version nodes already
  exist in `math/`, `resolv/` and `rt/` Versions files, and the abilists
  currently end at `GLIBC_2.43`.
- **Build system**: `Makeconfig` — `all-subdirs` (line 1441, new subdirs
  are added there and ordered by `sysd-sorted`); compile flags `-std=gnu11
  -fgnu89-inline` (line 1133), `+gccwarn = -Wall -Wwrite-strings -Wundef`
  (line 933, `-Werror` only under `--enable-werror`), `-fno-common`, stack
  protector; `-ftls-model=initial-exec` for `libc`/`rtld`/`libpthread`
  modules (lines 1105-1107); per-object flags via `CFLAGS-<file>.o`;
  object rules are pattern rules `$(objpfx)%$o: %.c` subject to `vpath`
  (`Makerules:415`, vpath precedent at `Makerules:69`); `Depend` files order
  cross-subdir header generation (`stdlib/Depend` = `localedata`).
- **Tests**: per-subdir `tests := tst-*`; `make check` = two-pass test run
  + `tests-special` (incl. `check-installed-headers-c/-cxx` which compile
  every installed header in many C/C++ modes — new header content must be
  clean there — and `check-local-headers`) + `check-abi`. Fork CI
  (`.github/workflows/ci.yml`, added by the fork's last commit):
  ubuntu-latest, `actions/checkout` (no submodules), install
  build-essential/bison/gawk/python3, `../configure --prefix=/usr`, `make
  -j$(nproc)`, `make -j$(nproc) check` with the summary extracted.
- **Threading/TLS**: nptl provides pthreads in libc since 2.34; glibc's own
  TLS vars in libc.so are initial-exec (see above);
  `__cxa_thread_atexit_impl` is exported (`GLIBC_2.18`, declared in
  `include/stdlib.h:165`) — the Itanium-ABI thread-exit registration
  primitive the reference implementation prefers.

### wg14_signals capabilities

- Public headers (`include/wg14_signals/`): `config.h` (embedder override
  layer: `WG14_SIGNALS_SIGACTION/ABORT/KILL_SELF/GETTID`, the mem/sigset/
  pthread-key/setjmp hooks, TLS policy, visibility, the
  `WG14_SIGNALS_DISABLE_SIGFENCE_MACRO` and
  `WG14_SIGNALS_DISABLE_SIGGUARDED_FAILURE_VALUE` embedder guards),
  `thrd_signal_handle.h` (the N3924 API: `sigfillset_*`x3, `sigguarded`,
  `sigdecider_abandon{,resume}`, `sighandle`, `siginstall`,
  `siguninstall`, `siguninstall_system`, `signal_decider_create/destroy`,
  `siginstall_set_sa_flags_np`, `siginstall_set_default_action_np`, the
  `sigfence` variadic macro, `SIGGUARDED_FAILURE_VALUE`, types
  `stdc_siginfo`, `stdc_siginfo_value`, `stdc_siginfo_error_code_t`,
  `stdc_siginfo_siginfo_t` (aliases `siginfo_t` on `__GLIBC__`),
  `stdc_siginfo_context_t` (`ucontext_t`), `sig_func_t`, `sig_recover_t`,
  `sig_decide_t`, `enum sig_decision`, `sig_default_action_np_t`),
  `tss_async_signal_safe.h` (`tss_async_signal_safe_t`,
  `tss_async_signal_safe_attr`, the four 7.30.6.5-8 functions),
  `current_thread_id.h` (`thread_id_t`, the `current_thread_id_cached`
  initial-exec TLS cache var, `static inline current_thread_id()`, and the
  exported `internal_current_thread_id_cached_set`).
- Implementation: `.ipp` files in `include/wg14_signals/detail/impl/`
  (`thrd_signal_handle_posix.c.ipp` 685 lines, `thrd_signal_handle_common.
  ipp.ipp`, `tss_async_signal_safe.c.ipp`, `current_thread_id.c.ipp`,
  `thread_atexit.c.ipp`, plus `linked_list.h`, `lock_unlock.h`,
  `verstable.h`); `src/wg14_signals/*.c` are one-line includes of the
  `.ipp`s. The library is pure C11.
- Include topology: a `.c` source includes
  `"wg14_signals/detail/impl/<name>.c.ipp"` — resolved with `-I
  <repo>/include`; the `.ipp` then pulls `"../../thrd_signal_handle.h"`
  etc. relatively. The `.ipp`s include only standard headers
  (`<signal.h>`, `<pthread.h>`, `<setjmp.h>`, `<stdlib.h>`, `<string.h>`,
  `<errno.h>`, `<stdbool.h>`, `<stdint.h>`, `<stdatomic.h>`, `<assert.h>`,
  `<stdio.h>`, `<unistd.h>`) — **not** `<threads.h>`, so glibc's extended
  `<threads.h>` never conflicts with the reference headers inside the
  library's own translation units.
- POSIX backend requirements: `sigaction`, `pthread_kill`/`pthread_self`,
  `NSIG`, `siginfo_t` with `si_addr` etc., `ucontext_t`, `setjmp`/`longjmp`
  (`_setjmp` preferred via `WG14_SIGNALS_HAVE__SETJMP`), `malloc`/`free`,
  `memcpy`/`memset`, `abort`, `__cxa_thread_atexit` (preferred) or the
  pthread-key fallback, `syscall(SYS_gettid)` on Linux, C11 atomics. Raw
  handler flags default to `SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER`
  (`thrd_signal_handle_posix.c.ipp:653`).
- The standalone suite (`test/*.c`, 40 tests as of 2026-08-27) already
  runs **on glibc hosts** (the llvm-libc plan records 38/38 on glibc
  aarch64 pre-2026-08-27, 40/40 after the two new siginstall_* tests) —
  the machinery itself is proven against glibc's headers and libc; what is
  new is compiling the reference sources **into** glibc and exposing the
  API from libc.
- N3924 rev-5 wording (`docs/proposed-wording.md`): 7.14.1 sigfence +
  SIGGUARDED_FAILURE_VALUE; 7.14.2.5-10 siginstall/siguninstall/
  signal_decider_create/signal_decider_destroy/sighandle/stdc_raise; 7.14.3.1-3
  sigguarded/sigdecider_abandon/sigdecider_abandon_resume; 7.14.4
  requirements for signal handlers and deciders; 7.30.6.5-8 the four
  tss_async_signal_safe_* functions in `<threads.h>`. `current_thread_id`
  appears in **no** revision of the wording — it is the reference
  implementation's own API (llvm-libc chose to expose it in `<threads.h>`;
  we do the same, documented as an extension).

### The llvm-libc precedent (what "similarly" means)

The llvm-project fork (`plans/llvm-project-fork.md`) proved the integration
recipe:

1. **Submodule at the integration point** (`libc/src/signal/wg14`), pinned;
   fork-owned files in a **sibling** directory (`wg14-build/`) because git
   cannot track files inside a submodule directory.
2. **Embedder override layer upstreamed into the reference** (config.h
   hooks) so the submodule needs no fork-side patches; only the
   recursion-critical call-site families are overridden (`sigaction`,
   `abort`, `pthread_kill(pthread_self(), ...)`); everything else resolves
   naturally.
3. **Existing signal entrypoints are completely untouched** — no
   re-pointing, no registry routing (an early prototype was reverted).
   **Policy: internal libc NEVER calls `siginstall`/`siguninstall`/
   `signal_decider_*`/`sighandle`** — only external code does; libc merely
   works correctly when external code does (the kernel raise enters the
   wg14 filtering handler by definition, because the siginstall'ed handler
   IS the kernel disposition).
4. **`abort()` NOTHING-TO-DO**: with SIGABRT siginstall'ed, abort's kernel
   raise delivers into the wg14 raw handler with the real siginfo; unclaimed
   -> default action terminates; a `sighandle()`-prepend attempt was made,
   found redundant *and* harmful (double handoff), and reverted.
5. **SA_NOCLDWAIT stripped** in the embedder shim so POSIX
   wait()/zombie semantics survive.
6. **Tests ported** as libc-native tests; the standalone suite stays the
   primary correctness net for the machinery.
7. **Lessons learned**: `-Wno-*` flags needed for the vendored
   `verstable.h`; a `setjmp`/`longjmp` bridge must be a `musttail` tail call
   (frame-reuse defect) — **not applicable to glibc**, whose `setjmp`/
   `longjmp` are real strong symbols inside libc, so no bridge layer exists.

Differences that make the glibc integration simpler: glibc already has all
referenced symbols (`sigaction`, `pthread_kill`, `pthread_self`, `syscall`,
sigset helpers, `__cxa_thread_atexit_impl`, `_setjmp`) as real libc
entrypoints — no raw-syscall shim is required (except the SA_NOCLDWAIT
strip); there is no namespace/wrapper layer (the reference functions ARE
the entrypoints); the build system is make, not CMake (new subdir +
`all-subdirs`), and symbol export is governed by `Versions` + `abilist`
files rather than CMake target registration.

### The llvm fork's git history (what the template's fork actually committed)

Read 2026-08-31 from `llvm-project/` (remote `git@github.com:ned14/
llvm-project.git`, five fork commits on top of upstream `d98d12cb99b6`).
This is the ground truth the llvm-libc plan describes; the commit-by-commit
record adds several facts the glibc plan above must inherit:

1. **Commit sequence** (oldest first), with the wg14_signals pin each one
   moved to:
   - `78b9d2624c52` "Get libc building on Mac OS" (2026-08-20) — added the
     submodule at `libc/src/signal/wg14` pinned to
     `50042dad4eb376bc88f6d21568007e72c615f3a8`.
   - `e25504f5a5ea` "Enable CI on github." (2026-08-20) — CI plumbing
     (fork-specific; glibc's CI already exists, Phase 4 just adds
     `submodules: recursive`).
   - `4f44ca90e236` "wg14_signals should now be wired into LLVM libc."
     (2026-08-21) — pin bumped to `b2059fb4916314d8befaa6680296f2cb738bcb71`
     (the upstream embedder-override-layer commits, §"Embedder override
     layer"), 79 files: `signal.yaml`/`threads.yaml`, the `llvm-libc-types`
     mirrors, `sigfence-macros.h` (the sigfence macro extracted verbatim),
     the `wg14-build/` fork-owned dir (CMake + `embedder_shim.{h,cpp}` +
     `wg14_hostcalls.cpp` + 17 thin entrypoint wrappers), `sigismember`
     (which glibc already exports).
   - `18160ae9cda7` "Add tests of wg14_signal using LLVM libc as its libc."
     (2026-08-22) — 7 hermetic test ports: `wg14_{raise_semantics,
     install_semantics,sigguarded,handoff,thread_handle,tss,chain}_test.cpp`
     (the proven subset the Phase 3 port list below mirrors).
   - `aace6638bf91` "Replace LLVM signal handling with the WG14 signals
     implementation." (2026-08-26) — pin bumped to
     `d25ccd22a6ea86a1e1c54ddbfe9988b1103e9db9` (current upstream HEAD);
     LLVM's own `llvm/lib/Support` signal handling now runs on the N3924
     API (§"Consumer validation").
2. **The upstream pins track the override-layer work**: `50042da` (pre-
   integration), `b2059fb` (override hooks + `WG14_SIGNALS_DISABLE_*`
   guards + the three `siginstall_*` np functions + new tests),
   `866d85d` (second integration round: `siginstall_default_action_test.c`,
   `siginstall_sa_flags_test.c`, `sighandle_noinstall_test.c` — the suite
   going 38 → 40), `d25ccd2` ("Fix CI Windows"), `77f4fbd` (split the old
   `stdc_raise()` into `sighandle()` plus a reserved `stdc_raise()`) and
   `2217d72` ("Implement `stdc_raise()` with the raw_info/raw_context
   contract"; the current glibc and llvm pins).
3. **`WG14_SIGNALS_PREFIX`**: the llvm fork compiled the submodule with
   `-DWG14_SIGNALS_PREFIX(x)=wg14_signals_##x` (config.h:26) so the
   submodule's public symbols could not collide with llvm-libc's own
   unprefixed entrypoint wrappers in `libc.a`. glibc needs **no prefix** —
   the reference functions ARE the entrypoints (no wrapper layer) — but the
   Phase 1 `nm -D` check must also confirm no prefixed or duplicate
   unprefixed symbols leak (§Phase 1 step 5).
4. **The llvm shim overrides every host-call family** (SIGACTION, ABORT,
   KILL_SELF, GETTID, MEMCPY/MEMSET/MALLOC/CALLOC/FREE, SIGEMPTYSET/
   SIGFILLSET/SIGADDSET/SIGDELSET/SIGISMEMBER, PTHREAD_KEY_CREATE/PTHREAD_
   ONCE/PTHREAD_SETSPECIFIC/PTHREAD_GETSPECIFIC, STDERR_PRINTF, SETJMP/
   LONGJMP) and implements raw-syscall replacements, because llvm-libc's
   entrypoints are C++-namespace-scoped and its test builds needed weak
   bridges. glibc overrides **one** hook (sigaction, SA_NOCLDWAIT strip);
   everything else resolves naturally — the minimal-override proof in
   §"Embedder override layer".
5. **SA_NOCLDWAIT strip is unconditional in the llvm embedder too**
   (`embedder_shim.cpp`, "must strip the flag"): the fork's `siginstall_set_
   sa_flags_np` cannot restore it there either. The glibc plan therefore
   documents SA_NOCLDWAIT as permanently stripped on glibc (Risk 2; Open
   question 3 resolves the strip as policy), with all other np-set flags
   (SA_NODEFER/SA_ONSTACK/SA_SIGINFO) effective.

### Consumer validation (what the fork's final commit proves)

`aace6638bf91` rewired `llvm/lib/Support`'s `Signals.inc`,
`CrashRecoveryContext.cpp`, `Program.inc` and `InitLLVM.cpp` onto the N3924
API — the first real-world consumer. The patterns it established are the
consumer contract the glibc fork must honour (each maps to a Phase 3 test):

- **np-setters run before the first `siginstall()` and apply to every later
  install**: `siginstall_set_sa_flags_np(SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER|
  SA_ONSTACK)` + `siginstall_set_default_action_np(...)` in
  `RegisterHandlersThreadsafe()` (`Signals.inc`; on glibc the embedder drops
  the SA_NOCLDWAIT bit — §"The llvm fork's git history" item 5). Signals
  installed before a setter call keep their old flags. → `tst-wg14-sa-flags-np`
  (new; port `siginstall_sa_flags_test.c` + an altstack arm:
  `sigaltstack()` then the np-set `SA_ONSTACK`, delivery on the alt stack)
  and `tst-wg14-default-action`.
- **siginfo-preserving default action**: the consumer's default-action
  callback resets to `SIG_DFL` and re-delivers via `SYS_rt_tgsigqueueinfo`
  (falling back to `raise()`), so the core dump keeps the faulting address.
  The reference documents exactly this in `sig_default_action_np_t`'s
  contract. glibc tests must **require** the siginfo-preserving path
  (`tst-wg14-default-action`: verify `si_addr`/`si_code` fidelity through a
  SIGSEGV default-action handoff) — the plan previously made it optional.
- **teardown order**: deciders are `signal_decider_destroy`'d **before**
  `siguninstall` so no decider node dangles off a live raw-handler entry —
  both in the failure path and in `unregisterHandlers()`. → ported
  `decider_destroy_return_test.c` already covers this; keep the ordering
  explicit in the test.
- **`siginstall` is refcounted per signal and coexists per-signal**:
  CrashRecoveryContext's `siginstall(&{SIGABRT,SIGBUS,SIGFPE,SIGILL,SIGSEGV,
  SIGTRAP})` runs alongside Signals.inc's installation of the same signals;
  the first install captures the pre-LLVM disposition, the last `siguninstall`
  restores it. → `tst-wg14-thread-handle` + a new coexistence arm in
  `tst-wg14-install-semantics` (two siginstall'ers on overlapping sets;
  verify the second uninstall leaves the first's handler in place).
- **incremental late install**: `SetOneShotPipeSignalFunction()` installs
  SIGPIPE on demand if the original registration ran without a pipe
  function. → arm in `tst-wg14-install-semantics`.
- **`sighandle()` walks the calling thread's guard frames directly and does
  not need the raw handler installed** (HandleExit uses it under a guard;
  the doc comment's "setup" form `sighandle(0, NULL, NULL)` initialises
  per-thread state). → `sighandle_zero_test.c` port already covers it.
- **`SIGGUARDED_FAILURE_VALUE` is the sigguarded() setup-failure sentinel**
  the consumer checks by value (`Result.int_value == SIGGUARDED_FAILURE_VALUE
  .int_value`). → `tst-wg14-sigguarded` port checks the sentinel.
- **C++ consumer shape**: `sig_decide_t`/`sig_func_t`/`sig_recover_t` are
  plain C function pointers; C++ consumers use non-capturing lambdas with a
  unary `+` and carry caller state through `stdc_siginfo_value` (the CRCI
  pointer travels in `value.ptr_value` and is read back in the decider via
  `Rsi->value.ptr_value`). glibc's mirrors must keep the C linkage
  (`extern "C"` + `__THROW`-free exact signatures) so `check-installed-
  headers-cxx` proves the same shape compiles from glibc's headers.
- **The musttail/weak-bridge machinery is llvm-test-framework-specific**
  (weak `setjmp`/`longjmp`/`malloc`/`free`/`__assert_fail` in
  `wg14_hostcalls.cpp` exist only so hermetic test links resolve; the
  `-Winvalid-noreturn` pragma wraps the musttail longjmp bridge). glibc has
  no hermetic object world: tests link the real built libc. **Do not
  replicate any of it.**
- **End-to-end consumer option** (Phase 5, optional): the llvm fork now
  ships `LLVM_ENABLE_THREADSAFE_SIGNALS` (Linux default ON); building the
  llvm toolchain against the glibc fork's libc.so.6 exercises the whole
  N3924 surface from libc itself (`llvm/lib/Support` pulls
  `siginstall`/`siguninstall`/`signal_decider_*`/`sigguarded`/`sighandle`/
  `sigfillset_*`/the np-setters). A cheap smoke of that shape (a small
  consumer replicating `RegisterHandlersThreadsafe()`'s install sequence)
  is folded into `tst-wg14-chain`; a full LLVM build is optional.

## Platform matrix

| Platform | glibc signals today | wg14_signals backend | Replacement scope | Gate |
|---|---|---|---|---|
| Linux (all archs: aarch64, x86_64, x86_64-x32, arm, i386, riscv, powerpc, s390, sparc, mips, loongarch, etc.) | full | POSIX (CI-tested) | **full**: existing API untouched + all N3924 entrypoints | always on |
| Hurd (mach) | full (mach-message based: `sysdeps/mach/hurd/`, htl threads) | POSIX backend would need `syscall()`-free GETTID + mach sigaction through glibc's own entrypoints | **unknown**: no `syscall(SYS_gettid)` on Hurd; signal semantics are mach-port based | deferred (§Phase 6); the wg14 subdir must not break Hurd builds meanwhile |
| Windows | glibc does not target Windows | (SEH backend exists but is irrelevant here) | none | excluded |

Linux is the only first-class target: the reference implementation is
already CI-proven on glibc hosts, and glibc's own CI builds and tests
Linux only.

## Comprehensive review: glibc's internal signal usage and coexistence

glibc's own signal handling is far larger than llvm-libc's, but the
replacement surface is the same (the N3924 API is for *external* code;
internal libc never calls it). What matters is that internal machinery
keeps working when external code siginstalls:

1. **`abort()`** (`stdlib/abort.c`) — **NOTHING-TO-DO** (same conclusion as
   the llvm-libc fork, reached there after a reverted `sighandle` attempt).
   `abort()` raises SIGABRT via `raise()` -> `pthread_kill(self)` -> tgkill.
   With SIGABRT siginstall'ed, the kernel delivers into the wg14 raw
   handler, the decider chain runs (recovery via `sig_decision_call_recovery`
   longjmps out of `abort()` and never returns; a returning chain falls
   through to the SIG_DFL reset + re-raise + unblock path, which terminates
   — C11-compliant in all paths). Without siginstall, byte-identical to
   today. glibc's abort already satisfies the `WG14_SIGNALS_ABORT` hook
   contract ("resets to SIG_DFL before re-raising; must not loop"), so the
   hook **defaults to glibc's own `abort()` with no override**.
   - **Abort rwlock interplay (glibc-specific, analyse and test)**: glibc's
     `__sigaction` (`signal/sigaction.c:36-42`) takes the abort **write**
     lock for *every* SIGABRT call — queries and sets alike — and
     `__abort_lock_wrlock` first runs `internal_signal_block_all` (blocks
     only SIGCANCEL/SIGSETXID, not SIGABRT) and restores on unlock. The
     shim therefore routes the wg14 machinery's SIGABRT disposition
     queries/resets through the lock. Hazard: the wg14 default-action
     handoff resets the disposition *inside the raw handler* (async
     context); if the interrupted thread is inside `abort()` holding the
     wrlock, the handler blocks on the lock. This cannot deadlock: the
     holder's path from `stdlib/abort.c:85-97` unconditionally re-raises
     with SIG_DFL and unblocks SIGABRT before falling through to
     `ABORT_INSTRUCTION`/`_exit(127)` — the process dies, releasing
     everything. The window is also tiny (the lock is held only between the
     first `raise()` returning and the re-raise). **Validation point**
     (smoke test + `tst-wg14-abort`): a thread inside `abort()` (SIGABRT
     siginstall'ed and *blocked* in a second thread's mask so the first
     raise returns) while another thread raises SIGABRT into the raw
     handler and falls through to the default action — process terminates,
     no hang. If the analysis ever weakens, the escape hatch is routing
     only the handler-context reset through `__libc_sigaction` (the lock-
     free raw wrapper) instead of `__sigaction` — do not adopt pre-emptively.
    - Note the second raise is `__pthread_raise_internal(SIGABRT)`
      (`stdlib/abort.c:90`), not `raise()`; the SIG_DFL reset is
      `__libc_sigaction` (line 89, deliberately lock-free — abort holds the
      lock itself).
    - **Lock ordering with the wg14 state spinlock — no cycle** (verified
      against the reference): the only paths that nest locks are the
      install and uninstall machinery, which hold the wg14 state spinlock
      **across** `WG14_SIGNALS_SIGACTION` (`thrd_signal_handle_common.
      ipp.ipp:448-463` — `install_sighandler_impl` at 463 — and
      `:504-542` — `uninstall_sighandler_impl` at 527), so the order is
      *state-spinlock → abort-wrlock*. The default-action handoff
      (`invoke_sigaction`, `thrd_signal_handle_posix.c.ipp:245-266`), by
      contrast, deliberately runs with the state spinlock **released** (the
      raise path unlocks at `.ipp:531-533` before invoking it, and the
      reference comments at `.ipp:246-248` that the spinlock "must not be
      taken" in that async-signal-safe context), so it takes the abort
      wrlock without holding the spinlock. `abort()` holds the wrlock
      without ever touching the spinlock. Strictly one-directional, no
      cycle.
2. **`system()`** (`sysdeps/posix/system.c`) — **NOTHING-TO-DO (policy)**.
   Its `__sigaction(SIGINT/SIGQUIT, SIG_IGN, &old)` save and `__sigaction(...
   &old)` restore round-trip whatever disposition is installed — including
   a wg14 raw handler — so an external `siginstall` survives `system()`
   intact and POSIX `system()` semantics are preserved.
3. **`posix_spawn`** (`sysdeps/posix/spawn*`, `sysdeps/unix/sysv/linux/
   spawn*`) — **NOTHING-TO-DO**. It only manipulates the child's *mask*
   (POSIX_SPAWN_SETSIGMASK) and *defaults* (POSIX_SPAWN_SETSIGDEF, used by
   `system()`), never the parent's dispositions.
4. **nptl internal signals** (SIGCANCEL 32, SIGSETXID 33) — protected by
   `is_internal_signal()` in `__sigaction` (EINVAL, `signal/sigaction.c:28`)
   and `__pthread_kill` (EINVAL). The wg14 machinery's interaction with
   them on glibc is subtle (verified against `thrd_signal_handle_common.
   ipp.ipp:544-641`, `siginstall_null_set_test.c`, and glibc's own
   `signal/sigfillset.c`):
   - **`siginstall(NULL)`** (common.ipp.ipp:551-572) does a raw
     `WG14_SIGNALS_SIGFILLSET(ret)` — which on glibc resolves to glibc's
     **own** `sigfillset()`, and that function calls
     `clear_internal_signals(set)` (`signal/sigfillset.c:28`,
     `internal-signals.h:58-61`), deleting 32/33 before the wg14 code even
     sees the set. The reference then deletes the realtime range via
     `#if defined(SIGRTMIN) && defined(SIGRTMAX)` — on glibc
     `SIGRTMIN`/`SIGRTMAX` are *runtime* macros
     (`__libc_current_sigrtmin()`, `signal/signal.h:387-388`), evaluating
     to 34..64 (RESERVED_SIGRT = 2, `signal/allocrtsig.c:27`). The NULL
     set therefore installs exactly the standard signals 1..31 (minus
     SIGKILL/SIGSTOP), never 32/33 or the RT range.
   - The reference's own `#ifdef SIGCANCEL`/`#ifdef SIGSETXID` deletions
     (common.ipp.ipp:583-588) are **inert on glibc** — SIGCANCEL/SIGSETXID
     live only in `internal-signals.h`, never in installed headers — and
     matter only for libcs that expose the names. Do NOT "fix" this by
     defining the names in the shim; the protection on glibc comes from
     glibc's own `sigfillset()`.
   - **Explicit user set containing 32/33** (e.g. a hand-built set, or
     glibc's `sigfillset` plus a `sigaddset` of SIGCANCEL): the deletion
     ifdefs don't fire, so `install_sighandler(32)` runs
     `WG14_SIGNALS_SIGACTION(32, ...)` → shim `__sigaction` → EINVAL →
     `install_sighandler` returns false → **siginstall performs a full
     rollback of this call's already-installed signals and returns NULL
     with errno = EINVAL** (common.ipp.ipp:603-637, the analysis.md 3.8
     rollback). No partial install leaks.
   - **SIGKILL/SIGSTOP** are silently skipped in the install loop (both
     NULL and explicit paths, common.ipp.ipp:591-594).
   - This is exactly the N3924 J.3 implementation-defined item "which
     signal numbers are not installed by a call of siginstall": on glibc
     the answer is SIGKILL/SIGSTOP (skipped silently) + SIGCANCEL/SIGSETXID
     (excluded from NULL/default installs; an explicit set containing them
     fails the whole call with EINVAL) + the whole realtime range for the
     NULL default set. **Validation point** (tst-wg14-install-semantics):
     all three paths — NULL-set success with 32/33/RT excluded, explicit
     glibc-`sigfillset`-based success (32/33 absent because glibc's
     sigfillset cleared them), explicit hand-added SIGCANCEL → NULL +
     EINVAL with no partial installs left behind.
5. **timer_create / SIGTIMER** (= SIGCANCEL) — same protection as above.
6. **`raise`/`kill`/`killpg`/`sigqueue`/`sigwait*`/`sigprocmask`/
   `pthread_sigmask`/`sigaltstack`** — outside the N3924 replacement
   surface, untouched, and wg14 never calls them (the reference uses
   `sigaction`, `pthread_kill(self)`, and the sigset helpers only).
7. **`__libc_sigaction` GLIBC_PRIVATE entrypoint** (used by abort etc.) —
   untouched.
8. **setjmp/longjmp** — glibc's per-arch real symbols; the reference's
   `setjmp`/`longjmp` hooks resolve naturally (no bridge, no `musttail`
   hazard — the llvm-libc bridge defect cannot occur here).
9. **ld.so** — no signal handling; lazy binding of the new exported
   functions is the same as for any glibc function.
10. **fork()/exec()** — the decider registry and per-thread state are
    plain process memory; after fork the child sees a consistent snapshot
    (the standalone suite exercises fork+signal patterns against glibc
    already). Thread-exit de-registration via `__cxa_thread_atexit_impl`
    works in children as in parents.
11. **The wg14 global state lock is a userspace spinlock** (`lock_unlock.h:
    33-61`: relaxed atomic load + CAS loop; no syscalls, no blocking) — a
    reference-wide property the glibc embedding inherits, with two
    consequences to document:
    - **It is async-signal-safe to acquire** (that is why the raw handler
      can take it for the global-decider walk) — and it is also
      **process-level**: a thread spinning on it consumes a core while
      the holder runs on another core. Normal operation; short critical
      sections (map lookup + decider-list traversal only; deciders run
      **unlocked**, `.ipp:499-507`).
    - **Same-thread re-entry window**: with `SA_NODEFER` (in the library
      default flags, `.ipp:653`) a nested delivery of the same signal can
      re-enter `raw_signal_handler` while the outer invocation is inside
      the state-lock critical section → the nested `LOCK` spins forever.
      The window is a few dozen instructions (no user code runs under the
      lock), and a synchronous fault cannot occur there absent a library
      bug, but it is nonzero — the reference accepts this; the standalone
      suite's nested-delivery tests (`thrd_signal_handle`, nested via
      `kill(getpid(), signo)`) exercise the path. glibc inherits the
      property unchanged; no glibc-specific mitigation is planned (a
      per-signal mask during critical sections would cost a syscall pair
      per install/raise — not worth it, and the llvm-libc fork ships the
      same design).
    - **No interaction with glibc's abort rwlock** — see item 1's lock-
      ordering sub-bullet.
12. **NDEBUG**: glibc does **not** define NDEBUG globally (only a few
    isolated files do), so the reference's `assert`s stay live inside
    libc: the map invariants (common.ipp.ipp:111/121/375), the UNLOCK
    `assert(former == 1)` (`lock_unlock.h:59`), and the
    `sigdecider_abandon`/`_resume` misuse asserts (posix `.ipp:550/589`).
    This matches glibc's own convention; a violated assert reports via
    glibc's `__assert_fail` → `abort()` → the same
    `WG14_SIGNALS_ABORT`-contract path as the reference's hard-abort
    sites. No `-DNDEBUG` should be added for the wg14 objects.

**Rules inherited from the llvm-libc fork (2026-08-22 policy), restated for
glibc**: (a) internal glibc NEVER calls `siginstall`/`siguninstall`/
`signal_decider_create`/`signal_decider_destroy`; (b) internal glibc NEVER
calls `sighandle()` for a signal it raises via the OS — the raise enters
the wg14 machinery by definition when external code siginstall'ed, and a
manual `sighandle()` double-handoffs when a pre-existing handler returns.

## Baseline: test results before any changes (2026-08-31)

Recorded before any modification; re-run/record the same after every phase.
Any row that flips from pass to fail is a regression to resolve before
proceeding.

| Suite | Expected today | How to verify |
|---|---|---|
| wg14_signals standalone suite (this repo) | **40/40** since 2026-08-27 (38/38 recorded on Linux glibc aarch64 on 2026-08-20; + `siginstall_sa_flags_test` + `siginstall_default_action_test`); macOS arm64 native and Linux glibc CI legs run the same 40; 27/27 Windows subset under wine (per `plans/llvm-project-fork.md` §"Baseline" and the repo's CI) | `cmake --build build && ctest --test-dir build -E benchmark` (per `.github/workflows/ci.yml`) |
| glibc fork CI (`make check` on ubuntu-latest) | **FAILS on x86_64** — see "CI failure investigation: x86_64 ubuntu-latest (2026-09-01)": non-root is fine (aarch64 non-root rc 0); `check-localplt` fails (fixed in `sysdeps/x86_64/localplt.data`, uncommitted); the remaining fail set is confirmed per-run by the rewritten workflow's exact breakdown (run page) | push to the fork or run `../configure --prefix=/usr && make -j && make -j check` on a Linux host |
| Local `make check` on the current host | TBD (this host is macOS; glibc does not build natively here — use the fork's GitHub CI as the baseline) | CI |
| `check-abi` | must pass with the pre-existing abilists | part of `make check` |
| glibc's signal tests (`signal/tst-*`, `nptl/tst-signal*`, `nptl/tst-kill*`, `sysdeps/pthread/tst-raise1`, `sysdeps/pthread/tst-pthread-exit-signal`, `sysdeps/pthread/tst-pthread_kill-exited`, `sysdeps/pthread/tst-pthread-raise-blocked-self`; abort has no dedicated `tst-abort*` in this tree — its behaviour is covered by the raise/kill tests and `debug/` backtrace tests) | all pass | part of `make check` |

## Linux test-suite comparison: baseline vs integration commit (2026-09-01, aarch64)

The complete `make check` was run twice on a Linux **aarch64** container
(glibc-builder image: ubuntu 24.04, gcc 13.3, the fork CI's exact
configure/make recipe, `-j4` for the 7.75 GiB Docker VM):

| Commit | `make check` | PASS | FAIL | XPASS | XFAIL | UNSUPPORTED | Total test-result files |
|---|---|---|---|---|---|---|---|
| `0d5632ba0e` "Have github CI run the test suite." (pre-integration baseline) | **rc 0** | 6986 | **0** | 2 | 16 | 144 | 7148 tests (14326 result files incl. conform/lint entries) |
| `51aa7e2ef1` "First round of integrating wg14_signals." **(as committed — unmodified)** | **build FAILS** | — | — | — | — | — | — (never reached `make check`) |
| `51aa7e2ef1` + the 10 working-tree fixes below (the fork's own files) | **rc 0** | 7001 | **0** | 2 | 16 | 144 | 7163 tests |

Conclusions:

1. **The integration commit as committed does not build on Linux
   aarch64** — the wg14 subdir fails to compile `tss_async_signal_safe.o`
   in a cascade of `-Werror` errors from the shim force-include being
   processed before glibc's libc-modules.h/libc-symbols.h force-includes
   (work item B1 below). The plan's §"Build wiring" ordering assumption
   was wrong in practice; its macOS `-fsyntax-only` harness (synthetic
   headers) could not catch it.
2. **With the 10 working-tree fixes (all in fork-owned files; the
   submodule stayed zero-drift), the integration is byte-identical in
   test outcomes to the baseline**: 6986 pre-existing tests all pass
   unchanged (0 status flips), and all 15 new wg14 tests pass, including
   the `check-wg14-mirror` sync check. `check-abi` (the hand-added
   aarch64 `GLIBC_2.44` abilist lines) passes. No regressions in signal,
   nptl, stdlib, posix, or debug suites.
3. **`stdlib/test-bz22786` timed out once (checked-in log from run v16's
   predecessor) and passed in every other run** — environmental (VM
   memory pressure during the parallel check), not an integration
   regression; the `Summary of results` block is identical to baseline
   in every other respect.
4. **Run environment notes** (this host): Docker Desktop's virtiofs
   bind mount proved incoherent for this workload (stale reads of
   freshly-written host files — phantom `sldlfcn/dladdr.os` /
   `nsunrpc/auth_none.os` members in `ar`, a `libc-misc.h` that vanished,
   a build log with binary junk; plus container-created files invisible
   to the host). All builds were therefore done with the source copied
   to the container-local fs, driver files baked into the image
   (`glibc-builder2`, `/opt/fix/`), and results copied back with an
   md5-verified manifest. Reproduce via `.kilo/tmp/glibc-check-run.sh`
   (the current version cp's the baked fix set for the `51aa` run name;
   a fixed baseline run is the same script with the baked files unused).

### CI failure investigation: x86_64 ubuntu-latest (2026-09-01)

Both fork CI runs failed `make check` with exit code 2 (run
33423638156 on master @ `0d5632ba0e`; run 33495930747 on
`replace_signals_handling` @ `561d74974c`), while the aarch64 Docker
runs passed. The failure was reproduced locally with the fork CI's exact
recipe, and the variables isolated:

| Environment | Commit | `make check` | Result |
|---|---|---|---|
| aarch64, root, Docker (`glibc-builder2`, ubuntu 24.04, gcc 13.3) | `561d74974c` | **rc 0** | 6886 PASS / **0 FAIL** / 2 XPASS / 16 XFAIL / 144 UNSUPPORTED |
| aarch64, **non-root** (`runner`, uid 1001, no sudo), Docker | `561d74974c` | **rc 0** | 6871 PASS / **0 FAIL** / 2 XPASS / 16 XFAIL / 159 UNSUPPORTED |
| x86_64 (**Rosetta emulation**), non-root, Docker | `561d74974c` | **rc 2** | 6951 PASS / **156 FAIL** / 16 XFAIL / 304 UNSUPPORTED |

Findings:

1. **Non-root is NOT the CI failure cause.** The suite passes cleanly as
   an unprivileged user (glibc's own INSTALL recommends it): the only
   status flips vs root are 15 privilege-requiring tests moving
   PASS→UNSUPPORTED (setuid/chroot/nss-files tests; `io/tst-fchmod-errors`
   flips the other way). The GitHub runner's blocked user namespaces map
   container tests to UNSUPPORTED (support/test-container.c
   `FAIL_UNSUPPORTED` on `unshare` EPERM), not FAIL.
2. **x86_64 `check-localplt` FAILS — real, fork-caused (W14).** The wg14
   objects introduce exactly the same 8 PLT references on x86_64 as on
   aarch64 (`pthread_kill`, `pthread_self`, `_longjmp`,
   `__cxa_thread_atexit_impl`, `thread_atexit`,
   `signal_decider_destroy`, `sighandle`,
   `internal_current_thread_id_cached_set`; the sigset helpers /
   sigaction / `__libc_current_sigrtmin/max` bind hidden via the internal
   `<signal.h>` shim include). `sysdeps/x86_64/localplt.data` only
   sanctioned malloc/matherr; the same 8 entries as the aarch64 file
   were added (committed in the working tree, uncommitted at the time of
   writing).
3. **All 15 wg14 tests + `check-wg14-mirror` PASS on x86_64** — the
   integration itself is clean; the x86_64 CI failure is not a wg14
   defect outside localplt.
4. **Of the 156 x86_64 FAILs, all but `check-localplt` are Rosetta/VM
   emulation artifacts** with explicit mechanisms: `math/test-{ldouble,
   float64x}-*` and `test-fenv*` (x87 extended-precision excess-precision
   ulps, e.g. `test-ldouble-acosh` differs in the last 3 mantissa bits),
   `elf/tst-cpu-features-cpuinfo*` (emulated `/proc/cpuinfo` vs CPUID
   mismatch), `elf/tst-execstack-*` (executable-stack emulation),
   `elf/tst-map-32bit-*` (`MAP_32BIT` hint ignored under emulation —
   DSO at 0x7fffff7ba2c0), ~57 container tests failing on a broken
   `testroot.root` (`readlink .../usr/bin/ld.so: No such file`) because
   unprivileged userns is *allowed* in Docker Desktop, `malloc/tst-*`
   and fortify tests trapping (VM address-space/overcommit), `posix/
   tst-spawn*` ("posix_spawn: Success" for a nonexistent binary),
   pthread timeouts (`tst-cancel31`, `tst-mallocfork3` — slow emulation),
   and `timezone/tst-tzset` (`setenv failed: Cannot allocate memory`
   under -j4 memory pressure). None of these ran on real x86_64
   hardware; a real-hardware confirmation of the remaining set (beyond
   localplt) is pending the next CI run — the CI workflow now prints the
   exact FAIL list (see below).
5. **The CI workflow never showed the summary**: GitHub's bash wrapper
   (`set -e -o pipefail`) aborted the step the moment `make check`
   returned 2, before the `sed` that prints "Summary of results". The
   workflow is rewritten to capture make's rc, always print an exact
   PASS/FAIL/XPASS/XFAIL/UNSUPPORTED/UNRESOLVED/ERROR breakdown (from
   the merged `tests.sum`, falling back to raw `.test-result` files),
   list the failing test names, write the table to the job summary
   (`$GITHUB_STEP_SUMMARY`, rendered on the run page), emit one
   `::error::` annotation, upload the failing tests' `.out` diagnostics,
   and exit with make's rc.

Reproduction scripts: `.kilo/tmp/glibc-nonroot-check-run.sh` (aarch64
non-root, commit 561d74974c) and `.kilo/tmp/glibc-x86_64-check-run.sh`
(x86_64 via Docker `--platform linux/amd64` Rosetta emulation;
`glibc-builder2-amd64` image). Build dirs copied back:
`glibc/build-nonroot/` and `glibc/build-x86_64/`.

### Work items raised by the comparison (all fixed; committed 2026-09-01
### as `561d74974c` "Second round of integrating wg14_signals" on the
### `replace_signals_handling` branch, pushed — the verified states below
### are the commit's contents, re-verified green by a full `make check`)

Build-blocking (the commit as committed cannot build):

- **W1 (DONE in worktree) — shim force-include order**: `wg14/Makefile`
  must not deliver `-include $(..)wg14/embedder_shim.h` via
  `$(subdir)-CPPFLAGS` (it precedes the libc-modules.h/libc-symbols.h
  force-includes in the CPPFLAGS composition, Makeconfig:1123-1131) —
  the shim's own `#include <signal.h>` then parses the internal headers
  before `IS_IN`/`libc_hidden_proto`/`rtld_hidden_proto` are defined,
  cascading `-Werror` failures. Use the per-basename slot
  (`CPPFLAGS-current_thread_id` etc., appended after the libc
  force-includes, Makeconfig:1130) for the four routine objects **only**:
  the tests must NOT get the shim, because its
  `WG14_SIGNALS_GLIBC_EMBEDDED` define excludes the mirror's
  type/typedef/declaration block from `<signal.h>` — the tests need that
  block (14 tests failed to compile with "declared inside parameter
  list" / `sig_decision_resume_execution` undeclared when the shim was
  applied subdir-wide via `CPPFLAGS-.o/.os`).
- **W2 (DONE in worktree) — `-Wundef` carve-out**: `config.h:80` tests
  `#if __STDC_VERSION__ >= 202300L || __cplusplus` with a bare
  `__cplusplus`; glibc's `-Wundef` is fatal via `-Werror`. Submodule is
  zero-drift, so `wg14/Makefile` gains `CFLAGS-.o += -Wno-error=undef`
  (and `.os`) — the plan's anticipated per-source `-Wno-*` carve-out;
  only the two builtin macros `__cplusplus` (this) need it (all other
  numerically-tested macros are shim-defined or glibc-header-defined:
  verified by scanning the submodule for bare `#if` uses).
- **W3 (DONE in worktree) — GETTID override is REQUIRED, not "natural"**:
  on aarch64 (asm-generic syscall list) glibc's `<bits/syscall.h>` maps
  no `SYS_gettid`, so the reference default `syscall(SYS_gettid)` does
  not compile inside libc (the plan's "natural, no override" claim was
  wrong). The shim now defines `WG14_SIGNALS_GETTID() __gettid ()` —
  glibc's own hidden thread-id wrapper (syscalls.list:108,
  `libc_hidden_proto` in `include/bits/unistd_ext.h`), async-signal-safe
  and PLT-free. Update the §"Embedder override layer" table's GETTID row
  and §"Comprehensive review" accordingly. (The standalone suite is
  unaffected — it builds against installed headers.)
- **W4 (DONE in worktree) — shim must include the INTERNAL `<signal.h>`**:
  the original `#include <signal/signal.h>` pre-defined `_SIGNAL_H`,
  silently disabling the internal header's hidden-prototype and
  sigsetops sections in every wg14 TU, so the sigset helpers,
  `__libc_current_sigrtmin/max`, `raise` etc. went through the PLT
  (`elf/check-localplt` reported 16 extras). With the shim now after
  libc-symbols.h, `#include <signal.h>` (the internal one — it
  re-includes the public and declares `libc_hidden_proto` for the sigset
  helpers, `__sigaction`, `__libc_current_sigrtmin/max`) makes those
  bind `__GI_*` hidden (verified in the built objects' `nm`).
- **W5 (DONE in worktree) — `check-localplt`**: the remaining legit PLT
  references (no hidden aliases exist for them) are sanctioned in
  `sysdeps/unix/sysv/linux/aarch64/localplt.data`: `pthread_kill`,
  `pthread_self`, `_longjmp`, `__cxa_thread_atexit_impl` (no
  `libc_hidden_proto` in `include/stdlib.h`), and the four internal
  cross-calls between wg14 objects (`thread_atexit`,
  `signal_decider_destroy`, `sighandle`,
  `internal_current_thread_id_cached_set`, all GLIBC_PRIVATE/public
  exports with no hidden alias in the zero-drift submodule). The other
  archs' localplt.data files get the same entries when their CI legs
  run (or use `make check` on each arch); aarch64 is proven.

Installed-header hygiene:

- **W6 (DONE in worktree) — mirror `_Static_assert` is invalid C++**:
  `signal/signal.h`'s sigfence argument-count assertion used
  `_Static_assert(...)` under a guard that included C++11+; C++ consumer
  TUs (glibc's own C++ tests, `test-math-isinff`, `tst-quick_exit`)
  failed with "expected constructor, destructor..." because
  `_Static_assert` is not a C++ keyword (the reference macro would have
  spelled `static_assert`; the mirror cannot, because the sync checker
  un-macros to `_Static_assert`). Guard is now C-only
  (`defined __STDC_VERSION__ >= 201112L && !defined __cplusplus`) —
  checker-safe because preprocessor lines are stripped before diffing.
- **W7 (DONE in worktree) — installed headers must be ASCII**: the
  mirror copied the reference's em-dash `—` characters; several
  `check-installed-headers-c/cxx` legs failed with "failure to convert
  ascii to UTF-8". Replaced with `--`. glibc convention: installed
  header comments are pure ASCII; the sync checker strips comments, so
  this does not affect the mirror match. Scan for non-ASCII again after
  any future mirror updates.
- **W8 (DONE in worktree) — `threads.h` mirror `static inline` is
  invalid in strict C89**: `current_thread_id()` declarations failed the
  nptl `check-installed-headers-c` c89 legs. The declaration is now
  gated `#if __STDC_VERSION__ >= 199901L || defined __cplusplus`
  (checker-safe as above; the reference implementation is C11-min so no
  functionality is lost — document in NEWS/manual that
  `current_thread_id()` needs C99+).
- **W9 (DONE in worktree) — `lint-makefiles`**: the `tests :=` list in
  `wg14/Makefile` must be alphabetically sorted (the lint rewrites and
  diffs it). Sorted.

Fork-owned test fixes (the wg14 ported tests, not the machinery):

- **W10 (DONE in worktree) — `tst-wg14-install-semantics` arm (c)
  semantics**: the plan's analysis ("SIGCANCEL/SIGSETXID deletion ifdefs
  are inert on glibc; an explicit set containing SIGCANCEL fails the
  call with EINVAL + rollback") is wrong **inside libc**: the wg14
  library objects' `<pthread.h>` resolves to the internal pthread
  headers (`sysdeps/nptl/pthreadP.h` → `internal-signals.h`), so
  `SIGCANCEL`/`SIGSETXID` ARE defined in the wg14 TUs and the
  reference's own "libc-internal signals are never installable"
  deletion fires (thrd_signal_handle_common.ipp.ipp:583-588) — the call
  succeeds with 32/33 silently excluded, exactly the reference's
  documented intent. The standalone suite keeps the EINVAL arm (public
  headers there). The test now expects the deletion semantics, and the
  plan's §"Comprehensive review" item 4 J.3 answer must be corrected
  (glibc-in-build = silent exclusion, not EINVAL).
- **W11 (DONE in worktree) — `tst-wg14-thread-handle` nested-arm
  counter**: expected `outer_decider_calls == 1`; the counter counts
  decider entries and the nested delivery runs the same decider a second
  time — the standalone `nested_decider_rsi_test.c` expects 2. Fixed to
  2.
- **W12 (DONE in worktree) — `tst-wg14-default-action`**: (a)
  `-Warray-bounds` at `-O2` on the deliberate faulting store —
  pragma-suppressed; (b) the si_addr verification was invalid: Linux
  `waitid()` on a reaped child returns a SIGCHLD record whose `si_addr`
  is not the faulting address (observed 0xc garbage while the callback
  correctly received `si_addr == 1` — proven with a probe against the
  built libc). The test now verifies fidelity via a MAP_SHARED page the
  callback writes before re-raising; the siginfo-preserving handoff
  itself is confirmed working.
- **W13 (DONE in worktree) — `tst-wg14-chain`**: missing
  `#include <stdlib.h>` for `abort()`.

Follow-up work items (not yet fixed):

- **W14 — environment/CI**: the x86_64 `check-localplt` half is **DONE
  in the worktree**: an x86_64 run of `561d74974c` shows exactly the
  same 8 extra PLT references as aarch64, now sanctioned in
  `sysdeps/x86_64/localplt.data` (same entries/comments as the aarch64
  file; not yet committed). The remaining half — confirming the CI's
  other failures against real x86_64 hardware — is pending the next CI
  run: the rewritten `.github/workflows/ci.yml` prints the exact
  PASS/FAIL/XPASS/XFAIL/UNSUPPORTED/UNRESOLVED/ERROR breakdown and the
  failing-test list into the run page (see "CI failure investigation"
  section above; every non-localplt failure observed in the local x86_64
  (Rosetta) run has an explicit emulation mechanism).
- **W15 — libc.a / static-link parity**: `make check` here exercised
  the shared libc path; the static `libc.a` build of the wg14 objects
  gets its own `make check` coverage when the CI leg runs (the 
  `link-libc-static` tests), which this run's manifest covers only
  partially (nptl/dlopen static tests passed, but an explicit CI run
  should confirm).
- **W16 — mirror/checker regression notes**: the checker normalisation
  (comments stripped) is what makes W6/W7/W8 possible without breaking
  the mirror sync check; document the two added gates in
  `check-wg14-mirror.py`'s header comment so future mirrors don't
  reintroduce the C++/C89/UTF-8 defects.
- **W17 — `stdlib/test-bz22786` flakiness**: observed one timeout in
  run v16's predecessor under VM memory pressure, passed in all other
  runs including baseline; keep an eye on it in CI (not an integration
  defect).

## Design

### Submodule integration (git submodule)

- Mount the reference implementation as a git submodule of the glibc fork
  at **`wg14_signals/`** (URL `https://github.com/ned14/wg14_signals.git`,
  **pinned to `2217d72eb0d1266eb8e97f2b5df0e83792b73867`** — current
  upstream `main` HEAD, the llvm fork's final pin; Appendix A). The
  submodule root IS the reference implementation: `include/`, `src/`,
  `test/`, `CMakeLists.txt`, docs. No copying, no subtree merge.
- All glibc-owned integration files live in the new top-level subdir
  **`glibc/wg14/`** (tracked by the fork; git cannot track files inside
  the submodule directory — the llvm-libc lesson). Contents:
  `Makefile`, `embedder_shim.h` (force-included), `Versions`, `Depend`,
  `tst-*.c` tests, and a sync-check script (Phase 3).
- Missing-submodule guard: the subdir's Makefile must fail with a clear
  error message and recovery instructions when `$(..)wg14_signals` is
  absent (a non-recursive clone) — mirror the llvm-libc CMake FATAL_ERROR
  pattern.
- CI must checkout with `submodules: recursive` (Phase 4). The nested
  `doc/html` submodule of wg14_signals (gh-pages branch) is not needed for
  the build and stays uninitialized.
- License compatibility: both projects are Apache-2.0; the submodule's
  license headers stay intact.

### Build wiring (the `wg14` subdir)

- Add `wg14` to `all-subdirs` (`Makeconfig:1441`).
- `glibc/wg14/Makefile` (model: `signal/Makefile`):
  - `routines := current_thread_id tss_async_signal_safe \
    thrd_signal_handle_posix thread_atexit` (the Windows backend
    `thrd_signal_handle_windows.c` is not compiled — glibc is POSIX-only).
    **The objects reach libc automatically**: each subdir's `routines`
    objects are archived per-subdir (`$(objpfx)stamp.o`/`stamp.os`,
    `Makerules:869-875`) and concatenated into `libc.a`/`libc_pic.a`
    (`Makerules:861-867`), which is what `libc.so` links — no extra wiring
    beyond `routines :=`. The sources live in the submodule; resolve them
    with `vpath %.c $(..)wg14_signals/src/wg14_signals` (glibc's object
    rules are pattern rules subject to vpath; `Makerules:415`, precedent at
    `Makerules:69`). If vpath proves unreliable, fall back to an explicit
    pattern rule in the subdir Makefile.
  - `CPPFLAGS`/`CFLAGS-<obj>.o`: add the shim force-include and the
    submodule include path via the subdir-wide hook
    **`wg14-CPPFLAGS = -include $(..)wg14/embedder_shim.h
    -I $(..)wg14_signals/include`** (`$(subdir)-CPPFLAGS`, `Makeconfig:
    1124`; precedent `locale/Makefile:174`) — this applies to every object
    (static `.o` and PIC `.os` alike) in the subdir, and to the subdir's
    tests too, where the shim's defines are harmless no-ops (tests only
    use the installed `<signal.h>` mirror, never the submodule's headers).
    Per-source overrides use the glibc-idiomatic `CFLAGS-<src>.c += ...`
    (`Makeconfig:1136`; e.g. `nptl/Makefile:221`). Note the wg14 objects
    are compiled with glibc's standard libc-object flags first:
    `-include $(common-objpfx)libc-modules.h` + `-DMODULE_NAME=$(in-module)`
    (module-cppflags, `Makeconfig:1109-1117`) and
    `-include include/libc-symbols.h` (`Makeconfig:1126`) — and
    `libc-symbols.h` itself defines `_GNU_SOURCE 1`, so the shim's own
    `_GNU_SOURCE` is belt-and-braces for the libc objects and load-bearing
    for the subdir's tests (which do not get libc-symbols.h). Multiple
    `-include` flags coexist in command-line order, the shim (from
    `Makeconfig:1124`) preceding the glibc ones — the shim's defines
    therefore still precede the `.ipp`'s first system include. The glibc
    machinery defines `IS_IN(libc)` and the hidden-symbol helpers, none of
    which the reference uses; its plain extern functions keep default
    visibility and are exported per the Versions file. The shim is the
    sole *wg14-specific* `-include`.
    The shim is also the sole home of every macro the `.ipp`s test
    numerically — `WG14_SIGNALS_SOURCE` (config.h:128),
    `WG14_SIGNALS_HAVE__SETJMP` (config.h:273,
    thrd_signal_handle_posix.c.ipp:44/51), and
    `#ifdef WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` (thread_atexit.c.ipp:53) —
    so `-Wundef` cannot fire (glibc's `+gccwarn` = `-Wall -Wwrite-strings
    -Wundef`, `Makeconfig:933`; `-Werror` only under `--enable-werror`).
    Compile mode is glibc's `-std=gnu11 -fgnu89-inline` (`Makeconfig:1133`,
    forced for every object): the reference is C11-clean, and
    `-fgnu89-inline` only alters *extern* inline semantics — the reference
    uses `static inline`/`static` throughout, so it is a non-issue (the
    plan's "expected fine" from Phase 1 step 3 is now confirmed against
    the actual flags). Add `-Wno-*` flags only if glibc's warning set
    trips on the vendored code (the llvm-libc build needed
    `-Wno-conversion -Wno-cast-qual -Wno-global-constructors` for
    `verstable.h` and the lazy-init sigset builders; glibc does not use
    `-Wconversion`, so likely fewer or none — the standalone suite builds
    clean under `-Wall -Wextra -Wpedantic -Werror`, llvm's own
    `LLVMwg14_signals` object library needed only `-Wno-conversion
    -Wno-cast-qual`).
  - `check-local-headers` (part of `make check`): scans only the *installed
    include dir* in the object dependency files (`scripts/check-local-headers.
    sh`, the `includedir`-prefixed match), so the submodule's relative
    includes (`../../thrd_signal_handle.h` etc.) — which resolve to the
    *source* tree, not the installed dir — cannot trip it. No exemption
    needed; re-verify in Phase 3.
  - `Depend`: `nptl` (verified: `nptl/Makefile:30` installs `pthread.h`; the
    objects include `<pthread.h>`; the installed `<threads.h>`-adjacent bits
    types also come from the nptl path via `sysdeps/pthread/`). `setjmp.h`
    is installed from the `setjmp/` subdir — add `setjmp` to `Depend` if the
    build orders headers before the wg14 objects need them (the `Depend`
    file lists subdirs that must have run first; `stdlib/Depend =
    localedata` is the pattern).
  - Objects default to `MODULE_NAME=libc` (`in-module`, `Makeconfig:
    1100-1110`), giving `-ftls-model=initial-exec`
    (`Makeconfig:1105-1107`) — exactly what the reference's TLS requires.
  - `headers :=` — none to install from this subdir (the API goes into the
    existing `signal.h`/`threads.h`; Phase 2 edits those subdirs' headers
    lists if needed — `threads.h` is already installed via
    `sysdeps/pthread/Makefile`).
  - `tests := tst-...` (Phase 3).
- The `embedder_shim.h` (force-included into the four wg14 TUs only):
  ```c
  /* glibc embedder shim for the wg14_signals submodule (N3924) */
  #define _GNU_SOURCE 1            /* expose NSIG, SIGSYS/SIGXCPU/SIGXFSZ,
                                      ucontext_t, syscall() in the headers;
                                      belt-and-braces for libc objects
                                      (libc-symbols.h already defines it),
                                      load-bearing for the subdir's tests  */
  #define WG14_SIGNALS_SOURCE 1    /* reference library build mode           */
  #define WG14_SIGNALS_HAVE__SETJMP 1        /* glibc provides _setjmp        */
  #define WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT 1
  #define __cxa_thread_atexit __cxa_thread_atexit_impl  /* glibc's GLIBC_2.18 */
  #define WG14_SIGNALS_DISABLE_SIGFENCE_MACRO 1          /* glibc <signal.h>
                                            provides the mirror               */
  #define WG14_SIGNALS_DISABLE_SIGGUARDED_FAILURE_VALUE 1 /* ditto           */
  #include <signal.h>  /* internal include/signal.h (via -I$(..)include):
                          declares __sigaction (:24) and __libc_sigaction
                          (:58) with libc_hidden_proto, so the wrapper
                          below needs no own declaration and binds to the
                          hidden alias (no PLT, no LD_PRELOAD interposition);
                          the DISABLE defines above are already in effect   */
  /* route raw-handler installs through glibc's own entrypoint, stripping
     SA_NOCLDWAIT so POSIX wait()/zombie semantics survive (llvm-libc
     precedent; N3924 does not specify the raw handler's sa_flags) */
  #define WG14_SIGNALS_SIGACTION(signum, act, oldact) \
    wg14_glibc_sigaction((signum), (act), (oldact))
  static inline int wg14_glibc_sigaction(int signum,
                                        const struct sigaction *act,
                                        struct sigaction *oldact)
  {
    struct sigaction act2;
    if (act != NULL)
      {
        act2 = *act;
        act2.sa_flags &= ~SA_NOCLDWAIT;
        act = &act2;
      }
    return __sigaction(signum, act, oldact);
  }
  ```
  (`__sigaction` resolves to glibc's internal entrypoint — the wg14 TUs
  are libc objects, so `-I$(..)include` (`Makeconfig:1085`) makes the
  internal `include/signal.h` the first `<signal.h>` they see: it declares
  `__sigaction` (`include/signal.h:24-26`) and `__libc_sigaction` (:58-60)
  with `libc_hidden_proto`, so the shim's call (and the `.ipp`s' `sigaction`
  hook calls, which bind the hidden sigset-helper aliases too) resolve to
  hidden symbols — no PLT, no `LD_PRELOAD` interposition. The shim's own
  `#include <signal.h>` (after its defines) provides the prototype in
  force-include order; the later `#include <signal.h>` in the `.ipp` is a
  guard no-op. The entrypoint validates signal numbers, rejects internal
  signals with EINVAL, and takes the abort **write** lock for SIGABRT
  (query and set alike, `signal/sigaction.c:36-42`) — the lock interplay
  is analysed in §"Comprehensive review" item 1 and covered by
  `tst-wg14-abort`.)
  Do NOT override `WG14_SIGNALS_ABORT` (glibc's `abort()` already meets
  the hook contract, §"Comprehensive review" item 1),
  `WG14_SIGNALS_KILL_SELF` (default `pthread_kill(pthread_self(), ...)`
  resolves to glibc's nptl tgkill entrypoint — async-signal-safe, no
  recursion), or `WG14_SIGNALS_GETTID` (default `syscall(SYS_gettid)`
  resolves to glibc's `syscall()`); all other hooks default to glibc's
  real entrypoints (Appendix B). SA_NOCLDWAIT is stripped **unconditionally**
  — `siginstall_set_sa_flags_np()` cannot restore it (the np-setter stores
  flags in the submodule, the strip happens later at the sigaction call
  site; the llvm-libc fork has the identical property, §"The llvm fork's
  git history" item 5); all other np-set flags are effective. Document in
  NEWS/manual (Phase 5) and pin with a test (`tst-wg14-sa-flags-np`).

### Embedder override layer: override vs natural (glibc)

| wg14_signals host call | glibc resolution | Embedding action |
|---|---|---|
| `sigaction` | `__sigaction` (validating, internal-signal-rejecting, abort-lock) | **OVERRIDE** via `WG14_SIGNALS_SIGACTION` -> shim wrapper that strips `SA_NOCLDWAIT` then calls `__sigaction` |
| `abort` | glibc `abort()` — resets SIG_DFL + re-raises before returning; cannot loop | **natural** (no override; contract met) |
| `pthread_kill(pthread_self(), ...)` | nptl tgkill entrypoint | **natural** (no override; contract met). Optional hardening (see below): override `WG14_SIGNALS_KILL_SELF` to `__pthread_raise_internal` |
| `syscall(SYS_gettid)` | glibc `syscall()` | **natural** (Linux; Hurd has no `syscall()` — Phase 6) |
| `getpid` | glibc `getpid()` | natural — glibc `libc_hidden_proto (getpid)` binds it hidden (no PLT); the new `WG14_SIGNALS_GETPID` hook defaults to it and is only reached from the default Linux `WG14_SIGNALS_RAISE_WITH_TOKEN` body |
| `getcontext(ucontext_t *)` | glibc `getcontext()` — no `libc_hidden_proto` | **OVERRIDE (new hook)**: the NULL-`raw_context` default capture (`WG14_SIGNALS_GETCONTEXT`). glibc's public `getcontext` is a weak alias of `__getcontext` with no internal hidden prototype, so leaving the default binds via the PLT; the fork currently sanctions `libc.so: getcontext ?` in `localplt.data`, but the preferred action is to point `WG14_SIGNALS_GETCONTEXT` at a hidden `__getcontext`-style alias |
| `memcpy`/`memset`/`malloc`/`calloc`/`free` | glibc's own; no signal calls | natural |
| sigset ops (`sigemptyset`/`sigfillset`/`sigaddset`/`sigdelset`/`sigismember`) | glibc's public sigset entrypoints (already exported) | natural (contrast llvm-libc, which had to add `sigismember`) |
| `setjmp`/`longjmp` (`_setjmp`) | glibc's per-arch real symbols | natural — no bridge, no musttail hazard |
| `__cxa_thread_atexit` | glibc exports `__cxa_thread_atexit_impl` (GLIBC_2.18) | shim defines `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` and maps the name to `__cxa_thread_atexit_impl` (pthread-key fallback excluded) |
| pthread-key fallback | (not compiled: cxa path used) | n/a |
| `errno`, `NSIG`, `ucontext_t`, `siginfo_t` | glibc headers; `_GNU_SOURCE` in shim exposes `NSIG`/`ucontext_t` | natural |
| `fprintf(stderr, ...)` | glibc stdio (bad-argument diagnostics only) | natural |

Only ONE hook is functionally overridden (sigaction, for the SA_NOCLDWAIT
strip); the rest are natural. This is the minimal-override proof: the
unmodified reference implementation extends glibc. The new
`WG14_SIGNALS_GETCONTEXT` hook (added upstream for the `stdc_raise()`
NULL-context path) is the one exception worth taking: overriding it to a
hidden `__getcontext` alias removes the `libc.so: getcontext ?` PLT
whitelist the fork currently carries, without changing behaviour.

**KILL_SELF optional hardening** (do not adopt pre-emptively): the
reference's KILL_SELF default `pthread_kill(pthread_self(), signo)` is
used at exactly one site — `default_action_impl`'s SIG_DFL re-delivery
(`thrd_signal_handle_posix.c.ipp:188`) — and is functionally correct on
glibc (pthread_kill → hidden `__pthread_kill` → tgkill; both
async-signal-safe). glibc's *own* abort, however, does not use
pthread_kill for its self-raise: it calls `__pthread_raise_internal`
(`stdlib/abort.c:90`), the direct `tgkill(getpid(), gettid(), signo)`
syscall (`nptl/pthread_kill.c:76-81`, `attribute_hidden` via
`sysdeps/nptl/pthreadP.h:513`, deliberately vfork-safe). An embedder that
wants parity with glibc's own choice can define
`WG14_SIGNALS_KILL_SELF(signo)` to `__pthread_raise_internal(signo)` in
the shim (it returns the errno value rather than -1/errno — the reference
ignores the return, `(void)` at `.ipp:188`). Keep the natural default:
the interposition surface is the same as glibc's own public-symbol calls,
and the minimal-override proof is the plan's headline property; revisit
only if a real interposition concern appears.

### Public API surface (installed headers)

**`signal/signal.h`** — add a `#ifdef __USE_GNU` block immediately before
`__END_DECLS` (line 393, after the `SIGRTMIN`/`SIGRTMAX` and
`bits/signal_ext.h` sections; there is no "sigsetops" section in this
header — the sigset helpers are declared earlier under `__USE_POSIX`),
mirroring the reference `thrd_signal_handle.h` declarations **verbatim**:

- The block first forward-declares the three internal structs the
  `stdc_siginfo` members reference (`struct sig_global_state_tss_state_
  per_frame_t`, `struct sighandler_info`, `struct global_signal_decider_t`)
  and includes `<stdbool.h>` (for `bool` in `stdc_siginfo`) and
  `<stdint.h>` (for `intptr_t` in `union stdc_siginfo_value`) — the
  reference header does both (`thrd_signal_handle.h:27-28`). `siginfo_t`
  (`bits/types/siginfo_t.h`, signal.h:57 under `__USE_POSIX199309`) and
  `ucontext_t` (`sys/ucontext.h`, signal.h:316, outer gate
  `__USE_XOPEN_EXTENDED || __USE_XOPEN2K8`, include itself under
  `__USE_XOPEN || __USE_XOPEN2K8`) are already in scope because
  `__USE_GNU` implies all of these.
- Types: `union stdc_siginfo_value` (intptr_t/void* members; C++ constexpr
  ctors under `__cplusplus`), `typedef int stdc_siginfo_error_code_t`,
  `struct stdc_siginfo` (signo, error_code, addr, value, raw_info
  (`siginfo_t *`), raw_context (`ucontext_t *`), the three
  `internal_*` members, `internal_decider_is_abandoned` bool — **all
  members, in order, layout-identical**), `typedef siginfo_t
  stdc_siginfo_siginfo_t`, `typedef ucontext_t stdc_siginfo_context_t`,
  `typedef union stdc_siginfo_value (*sig_func_t)(union
  stdc_siginfo_value)`, `typedef union stdc_siginfo_value
  (*sig_recover_t)(const struct stdc_siginfo *)`, `enum sig_decision
  { sig_decision_next_decider, sig_decision_resume_execution,
  sig_decision_call_recovery }`, `typedef enum sig_decision
  (*sig_decide_t)(struct stdc_siginfo *)`, `typedef void
  (*sig_default_action_np_t)(int, siginfo_t *, void *)`.
- `SIGGUARDED_FAILURE_VALUE`: mirror the reference spelling — `static
  const` union (C11/C17), `constexpr` (C23/C++) under the same
  `WG14_SIGNALS_C23_CONSTEXPR_OR_CONST`-style guard logic.
- The `sigfence(...)` variadic macro, extracted verbatim from the reference
  header (the `WG14_SIGNALS_SIGFENCE_*` machinery — the
  `WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8` argument-count dispatch with the
  compile-time `_Static_assert` (spelled glibc-style, not via the reference's
  `WG14_SIGNALS_STATIC_ASSERT`), the `__asm__ volatile(";" : "+m"(...))`
  path under `__GNUC__`/`__clang__`, and the `DISABLE_INLINE_ASM`/non-GNU
  volatile-sink fallback with its per-TU `static void *volatile
  sigfence_sink[9]` — rename the sink to a glibc `__`-prefixed internal name
  in the mirror, e.g. `__wg14_sigfence_sink`).
- Function declarations: `sigfillset_synchronous`, `sigfillset_
  asynchronous_nondebug`, `sigfillset_asynchronous_debug`, `sigguarded`,
  `sigdecider_abandon`, `sigdecider_abandon_resume`, `sighandle`,
  `siginstall`, `siguninstall`, `siguninstall_system`,
  `signal_decider_create`, `signal_decider_destroy`,
  `siginstall_set_sa_flags_np`, `siginstall_set_default_action_np` — exact
  signatures from the reference header (which match the proposal wording
  7.14.2.5-10, 7.14.3.1-3).

**`sysdeps/pthread/threads.h`** — add under `__USE_GNU` immediately after
`tss_delete` (line 233) and before `__END_DECLS` (line 235), per
7.30.6.5-8 plus the reference extension:

- `#include <stdint.h>` at the top of the block (the header currently
  includes only `<time.h>` + bits types; `thread_id_t` is `uintptr_t`).
- `typedef struct tss_async_signal_safe_s *tss_async_signal_safe_t;`
- `struct tss_async_signal_safe_attr { int (*const create)(void **dest);
  int (*const destroy)(void *v); };`
- `tss_async_signal_safe_create` / `tss_async_signal_safe_destroy` /
  `tss_async_signal_safe_thread_init` / `tss_async_signal_safe_get` —
  signatures from the reference header.
- `typedef uintptr_t thread_id_t;` + the reference's
  `static const thread_id_t thread_id_t_tombstone = 0;` + `current_thread_id()`
  as the reference's `static inline` (TLS cache read) + the extern
  `current_thread_id_cached` initial-exec TLS declaration +
  `internal_current_thread_id_cached_set` declaration — all mirrored
  verbatim (spell the TLS as `__thread ... __attribute__((tls_model(
  "initial-exec")))` for pre-C11 header consumers, semantically identical
  to `_Thread_local` under initial-exec).
- Gate the whole block with `__USE_GNU`.

**Feature-test gating**: `__USE_GNU` (glibc convention for extensions; the
conform tests check *presence* of standard symbols, not absence of GNU
extensions, and `check-installed-headers` compiles every header in strict
modes where `__USE_GNU` is off — validation point: run those checks).

**Sync test** (Phase 3): a `tests-special` script
(`glibc/wg14/check-wg14-mirror.py`) that extracts the N3924 declaration
blocks from the submodule's `thrd_signal_handle.h`/`tss_async_signal_safe.h`/
`current_thread_id.h` and diffs them against the mirror blocks in glibc's
`signal.h`/`threads.h` after a small normalisation (strip
`WG14_SIGNALS_PREFIX`/`WG14_SIGNALS_EXTERN`, un-macro the
`WG14_SIGNALS_C23_CONSTEXPR_OR_CONST`/`WG14_SIGNALS_STATIC_ASSERT` spellings
against their glibc-style equivalents, drop the `sigfence` sink rename), so
the mirror cannot drift from the reference (the llvm-libc open-question 1
sync-test idea, made a hard check here).

**Conflicts to expect and defuse**: in the wg14 library TUs, both glibc's
`<signal.h>` (via the `.ipp`'s `#include <signal.h>`) and the reference
`thrd_signal_handle.h` define the N3924 types/functions — duplicate
*identical* definitions are valid C, but the two macros (`sigfence`,
`SIGGUARDED_FAILURE_VALUE`) are not — hence the two
`WG14_SIGNALS_DISABLE_*` defines in the shim (the embedder-guard feature
the llvm-libc integration upstreamed for exactly this). Note that the
wg14 TUs' `#include <signal.h>` resolves to the **internal**
`include/signal.h` (superset of the public one — `_SIGNAL_H`-guard
protects against double-inclusion with the public header's own guard), so
the mirror declarations in the public `signal/signal.h` are present in
these TUs too, and the type/macro conflicts are exactly as analysed.

**Interposition immunity**: because the wg14 objects are compiled as libc
objects, every host call they make binds to a hidden alias where glibc
declares one — `__sigaction`, the sigset helpers (`libc_hidden_proto
(sigemptyset/fillset/addset/delset/ismember)`, `include/signal.h:7-11`),
`raise`/`__kill` — so no `LD_PRELOAD` or interposed definition can divert
the wg14 machinery's host calls. `pthread_kill`/`pthread_self` (the
KILL_SELF default) and `malloc`/`free`/`memcpy`/`memset` are plain
interposable symbols in libc.so (glibc's own code calls them the same
way), which is the established glibc model. This is a strictly stronger
property than the standalone build's and worth an explicit `nm -D`/`objdump`
spot check in Phase 1 (confirm no `wg14` object calls an interposable
`sigaction`).

### Symbol versioning and ABI

- New `glibc/wg14/Versions`:
  ```
  libc {
    GLIBC_2.44 {
      sigfillset_synchronous; sigfillset_asynchronous_nondebug;
      sigfillset_asynchronous_debug; sigguarded; sigdecider_abandon;
      sigdecider_abandon_resume; sighandle; siginstall; siguninstall;
      siguninstall_system; signal_decider_create; signal_decider_destroy;
      siginstall_set_sa_flags_np; siginstall_set_default_action_np;
      tss_async_signal_safe_create; tss_async_signal_safe_destroy;
      tss_async_signal_safe_thread_init; tss_async_signal_safe_get;
      internal_current_thread_id_cached_set;
      current_thread_id_cached;
    }
    GLIBC_PRIVATE {
      thread_atexit;
    }
  }
  ```
  (`current_thread_id_cached` is the initial-exec TLS data symbol the
  consumer-side inline reads; `thread_atexit` is the internal helper that
  is global (not static) in the reference — keep it out of the public ABI
  via GLIBC_PRIVATE, or hide it if the reference gains a visibility hook.
  Note the GLIBC_PRIVATE export is **required for the link, not just
  hygiene**: `tss_async_signal_safe.c.ipp:340` calls `thread_atexit()`
  directly, so the symbol must be resolvable between the four wg14 objects
  inside libc.so — the version script (libc.map, generated from the
  Versions files) hides everything not listed, and GLIBC_PRIVATE is the
  sanctioned way to keep a cross-object internal symbol versioned.)
- abilists: after the first Linux build, run `make update-all-abi-libc`
  (or hand-add) `GLIBC_2.44 <sym> F` lines to every
  `sysdeps/unix/sysv/linux/*/libc.abilist` (aarch64, x86_64/64, x86_64-x32,
  i386, arm, riscv64, powerpc32/64/le/el, s390/31/64, sparc32/64, mips*,
  loongarch, sh*, csky, arc, m68k, microblaze, or1k, alpha, hppa) plus
  `GLIBC_2.44 current_thread_id_cached D 0x8`-style data lines (copy the
  format of an existing TLS data symbol, e.g.
  `GLIBC_2.32 __libc_single_threaded D 0x1` in the x86_64 abilist; the `D`
  size is per-arch pointer width — 0x8 on 64-bit, 0x4 on 32-bit —
  `update-all-abi-libc` fills it in mechanically). `check-abi` (part of
  `make check`) fails without these.

### Internal consumers policy

Restate the llvm-libc policy verbatim for glibc (it survives unchanged
because glibc's internals never needed to call the wg14 API in the first
place): **internal glibc NEVER calls `siginstall`/`siguninstall`/
`signal_decider_create`/`signal_decider_destroy` — ONLY code outside libc
does. glibc merely works correctly when external code does so.** `abort()`,
`system()`, `posix_spawn()`, nptl: NOTHING-TO-DO, with the evidence in
§"Comprehensive review".

### TLS policy inside glibc

- `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL` defaults to 1 on
  GNU/non-Apple (config.h:56-57) — glibc qualifies. glibc already compiles
  libc's own objects with `-ftls-model=initial-exec`
  (`Makeconfig:1105-1107`), and the reference headers force
  `tls_model("initial-exec")` explicitly; libc.so is always in the initial
  link set, so the runtime-loaded-DSO caveat does not apply (glibc's own
  errno-style TLS has the same property).
- `current_thread_id_cached` and the per-thread signal state are
  initial-exec TLS in libc.so; consumer TUs accessing the cached var must
  use the same `tls_model("initial-exec")` attribute on the extern
  declaration (the mirrored header provides it).
- Static libc.a: initial-exec TLS in static binaries works (glibc's own
  static TLS machinery).

## Detailed phases

### Phase 1 — Submodule, build wiring, smoke test (Linux)

1. `git submodule add https://github.com/ned14/wg14_signals.git
   wg14_signals` in the glibc fork, pinned to
   `2217d72eb0d1266eb8e97f2b5df0e83792b73867` (current upstream HEAD, the
   llvm fork's final pin); commit the `.gitmodules` entry and the pin.
2. Add `wg14` to `all-subdirs` (`Makeconfig:1441`); create
   `glibc/wg14/{Makefile,embedder_shim.h,Depend,Versions}` per §"Build
   wiring" and §"Symbol versioning and ABI". Missing-submodule guard in
   the Makefile.
3. Configure and build on Linux (the fork CI recipe: `../configure
   --prefix=/usr && make -j$(nproc)`), on the fork's CI or a Linux host.
   Fix compile issues: `-Wundef` on any unguarded macro conditional
   (the shim defines the full set: `WG14_SIGNALS_SOURCE`,
   `WG14_SIGNALS_HAVE__SETJMP`, `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT`,
   the two `WG14_SIGNALS_DISABLE_*`), `-Wwrite-strings`, per-source
   `CFLAGS-<src>.c += -Wno-*` if the vendored `verstable.h` trips glibc's
   warning set (llvm needed `-Wno-conversion -Wno-cast-qual` for it;
   glibc's set is smaller — likely none). Compile mode is glibc's forced
   `-std=gnu11 -fgnu89-inline` (confirmed harmless: the reference is
   C11-clean and `-fgnu89-inline` only affects *extern* inline, of which
   the reference has none — all helpers are `static`/`static inline`).
4. Smoke test (fork-owned, e.g. `glibc/wg14/tst-smoke.c` run manually
   against the built libc, or the first ported test): link a small program
   against the built `libc.so.6`/`libc.a` that exercises, like the
   llvm-libc smoke test: existing sigaction/signal/raise/SA_SIGINFO/
   SIG_IGN/sigprocmask round-trips (untouched entrypoints), `siginstall`/
   `siguninstall`, `signal_decider_create`/`destroy`, `sighandle`,
   `sigguarded` SIGSEGV recovery, `sigfillset_*`, `current_thread_id`,
   `tss_async_signal_safe_*` (multi-threaded, thread-exit cleanup via
   `__cxa_thread_atexit_impl`), default-action delivery (SIGTERM kills the
   child; core-dump signals die by signal), `abort()` semantics with
   and without siginstall'ed SIGABRT, and the **abort-lock interplay**
   (a thread blocked in the raw handler's default-action reset while
   another thread is inside `abort()`; §"Comprehensive review" item 1).
5. Run `nm -D` on the built libc.so to confirm the 19+1 symbols are
   versioned `GLIBC_2.44`, `thread_atexit` is GLIBC_PRIVATE-only, and no
   unexpected unversioned globals leaked — in particular **no
   `wg14_signals_`-prefixed and no duplicate unprefixed symbols** (the
   llvm fork needed `WG14_SIGNALS_PREFIX` for its wrapper layer; glibc has
   no wrapper layer, so the unprefixed names ARE the exports — verify
   nothing else surfaced).
6. Run `make update-all-abi-libc` and commit the abilist changes.

### Phase 2 — Public headers

1. Extend `signal/signal.h` (N3924 block under `__USE_GNU`, verbatim
   mirror) and `sysdeps/pthread/threads.h` (tss block + `current_thread_id`
   under `__USE_GNU`, verbatim mirror) per §"Public API surface". Do NOT
   `#include` any `wg14_signals/` header from installed glibc headers —
   the mirrors are self-contained (the installed headers must not depend
   on the submodule).
2. Verify `check-installed-headers-c/-cxx` pass for `signal.h` and
   `threads.h` (all C/C++ modes glibc checks), and that a consumer TU
   `#define _GNU_SOURCE` + `<signal.h>` + `<threads.h>` compiles clean in
   C11 and C++.
3. Verify the `conform` data files need no change (the new symbols are
   GNU-gated extensions; conform checks presence, not absence — confirm
   with the conform suite run in Phase 3).
4. Confirm the two `WG14_SIGNALS_DISABLE_*` shim defines defuse the
   macro conflicts in the wg14 TUs (build is the proof).

### Phase 3 — Tests

Port the meaningful wg14_signals tests to glibc's harness as
`glibc/wg14/tst-*.c` (each `#define _GNU_SOURCE` first — the API is
`__USE_GNU`-gated; use glibc's `support/` test framework where helpful;
fork+waitpid in-test for death/recovery tests, never sleeps-alone for
synchronisation — repo AGENTS.md rule 5). All tests are plain C11
(AGENTS.md rules 1 and 4: no C++ under `include/`/`src/`; C++ coverage of
the headers comes from glibc's own `check-installed-headers-cxx`).
Suggested port list (naming mirrors the llvm-libc hermetic ports, which
are the proven subset; the §"Consumer validation" patterns map to arms
below):

- `tst-wg14-raise-semantics` — `sighandle_zero`, `sighandle_uninstalled`,
  `sighandle_null_info`, out-of-range signo; plus the consumer pattern
  that `sighandle()` walks guard frames directly (works without the raw
  handler installed).
- `tst-wg14-install-semantics` — `siginstall_null_set` (port; on glibc the
  NULL set's handle must exclude 32/33 — via glibc's own `sigfillset`'s
  `clear_internal_signals` — and the realtime range, while keeping the
  standard signals), `edge_api_coverage`, `sig_default_preserve`, plus the
  glibc-native three-path matrix from §"Comprehensive review" item 4:
  (a) NULL set succeeds; (b) an explicit set built with glibc's
  `sigfillset` succeeds and its handle excludes 32/33; (c) an explicit set
  with SIGCANCEL hand-added via `sigaddset` makes `siginstall` return NULL
  with errno = EINVAL **with a full rollback** (verify no partial installs
  remain — a subsequent `siginstall(NULL)` still works, and the pre-existing
  disposition of an earlier-installed signal in the failed set is
  untouched); (d) SIGKILL/SIGSTOP are silently skipped (the set
  `sigfillset_asynchronous_nondebug`, which contains them per the header
  docs, installs fine); plus the consumer coexistence arms: two
  `siginstall`ers on overlapping sets (refcounted, first-in captures the
  pre-install disposition, last-out restores it) and the incremental late
  install of a not-yet-installed signal.
- `tst-wg14-sigguarded` — recovery_null_loop, `sigguarded_tss_init`,
  `SIGGUARDED_FAILURE_VALUE` sentinel.
- `tst-wg14-sa-flags-np` (new) — port `siginstall_sa_flags_test.c`:
  np-setter applies to all later installs, earlier installs keep their
  flags, `SA_SIGINFO`-less flag sets fail with EINVAL, and the glibc
  specifics: `SA_ONSTACK` + `sigaltstack` delivers on the alt stack,
  `SA_NOCLDWAIT` is silently dropped by the embedder (waitpid still reaps;
  zombie semantics preserved).
- `tst-wg14-handoff` — `nsih_siginfo_handoff`, `decider_destroy_return`
  (keep the consumer's teardown order: `signal_decider_destroy` before
  `siguninstall`).
- `tst-wg14-thread-handle` — `thrd_signal_handle` (thread-local recovery,
  global claim, concurrent destroy; nested delivery via
  `kill(getpid(), signo)`), `nested_decider_rsi`.
- `tst-wg14-tss` — `tss_destroy_reentrancy` plus basics, multi-threaded
  with `thrd_create`/`thrd_join`, verifying thread-exit cleanup through
  `__cxa_thread_atexit_impl`.
- `tst-wg14-chain` — glibc-native: the consumer's `RegisterHandlersThread-
  safe()` shape (np-setters first, then `siginstall` + one decider per
  handler kind), user-handler wrap/claim/decline/restore round-trip,
  SA_SIGINFO fidelity through the handoff, and `abort()` re-entry
  recovering out of a `sigguarded(SIGABRT)` frame.
- `tst-wg14-sigfillsets` — the three `sigfillset_*` sets' contents and
  the sync/async/debug categorization on Linux.
- `tst-wg14-current-thread-id` — `current_thread_id` inline vs
  `gettid()`, per-thread distinctness.
- `tst-wg14-default-action` — default-action delivery: SIG_DFL reset +
  re-delivery semantics; `siginstall_set_default_action_np` with the
  siginfo-preserving callback via `SYS_rt_tgsigqueueinfo` **required**
  (port `siginstall_default_action_test.c`; the LLVM consumer relies on
  `si_addr`/`si_code` core-dump fidelity through the handoff — verify the
  callback receives the original `siginfo_t` and the re-delivered
  SIGSEGV's `si_addr` matches).
- `tst-wg14-abort` — abort with no siginstall -> SIGABRT death (wait status);
  abort inside `sigguarded(SIGABRT)` -> recovery; abort with a returning
  claim decider -> hard-abort termination; plus the abort-lock interplay
  arm: thread A blocks SIGABRT (`pthread_sigmask`, so `abort()`'s first
  `raise()` returns pending and it takes the wrlock), thread B (SIGABRT
  unblocked) concurrently raises SIGABRT through the raw handler's
  default-action path — the process must terminate promptly (a hang fails
  the ctest timeout; the wrlock window A holds is only between the first
  raise's return and the SIG_DFL re-raise).
- `tst-wg14-guard-abandon` — `sigdecider_abandon` /
  `sigdecider_abandon_resume` paths.
- `tst-wg14-sigfence` — `sigfence_fence_test` port (fence behaviour +
  macro argument-count dispatch).
- `tst-wg14-mixed-set` and `tst-wg14-reentrant-destroy` — optional
  (decider_mixed_set, decider_reentrant_destroy).

Also add the **mirror sync check** as a `tests-special` entry
(`check-wg14-mirror.py`, §"Public API surface"), and keep the white-box
tests (`raise_claim_cleanup`, `siginstall_rollback`, `sig_unknown_handler`,
`signo_map_verstable_init`, header-only family) out of glibc — they stay
covered by the standalone suite.

Run `make check` on Linux: every pre-existing glibc test must pass
unchanged (contract preservation), the new tests pass, `check-abi` passes
with the new abilists, `check-installed-headers` passes with the extended
headers, and the `conform` suite is unchanged. Record the CI "Summary of
results" block before and after.

### Phase 4 — CI

1. `.github/workflows/ci.yml`: `actions/checkout@v4` gains
   `submodules: recursive` (the wg14_signals `doc/html` nested submodule
   stays uninitialized — check that `make check` does not need it; it
   does not).
2. Keep `configure --prefix=/usr && make -j$(nproc) && make -j$(nproc)
   check`. ubuntu-latest is a glibc host, so the ported tests exercise the
   N3924 API against the fork's own libc.
3. Confirm the fork CI baseline first (record before Phase 1; §"Baseline").

### Phase 5 — Documentation

1. glibc: `NEWS` entry for the N3924 signal API; a manual node
   (`manual/signal.texi`-style, optional but recommended) describing
   `sigguarded`/`siginstall`/`sighandle` etc. and the glibc-specific
   notes: internal signals (SIGCANCEL/SIGSETXID) non-installable (N3924
   J.3); SA_NOCLDWAIT **permanently stripped** by the embedder (POSIX
   wait()/zombie semantics preserved) — `siginstall_set_sa_flags_np`'s
   other flags (SA_NODEFER, SA_ONSTACK, SA_SIGINFO) are effective, but
   SA_NOCLDWAIT is silently dropped on glibc; `current_thread_id`/
   `thread_id_t` are reference-implementation extensions (not in the
   N3924 wording).
2. This repo: `Readme.md` supported-targets table gains the glibc fork;
   `plans/glibc-fork.md` cross-referenced from `plans/llvm-project-fork.md`.
3. Optional consumer validation: build the llvm fork's toolchain with
   `LLVM_ENABLE_THREADSAFE_SIGNALS` (Linux default ON) against the glibc
   fork's libc.so.6 — LLVM's `llvm/lib/Support` then exercises the N3924
   surface from glibc itself (§"Consumer validation").

### Phase 6 — Hurd assessment and deferred work

- Hurd builds must not break: gate the `wg14` subdir out on Hurd until the
  backend is assessed. Concrete mechanism: in `glibc/wg14/Makefile`, guard
  the whole file with `ifeq ($(config-os),gnu)` (no `routines`, no
  `tests` — the subdir then contributes nothing but its `subdir := wg14`
  header). `config-os = @host_os@` (`config.make.in:37`), and the Hurd
  host triple's OS is `gnu` — verify the value in `config.make` on a Hurd
  configure at implementation time and use the exact spelling. (An
  `ifeq ($(subdir),...)` gate is NOT available here: sysdeps Makefile
  fragments are per-subdir, this is a top-level subdir, so the OS-level
  variable is the right test.) The missing-submodule guard (Phase 1 step
  2) must live **inside** the Linux branch: a Hurd build must not require
  the submodule to exist at all.
  Also gate the installed-header mirror blocks' *declarations* are
  self-consistent: on Hurd the wg14 subdir is absent, so `signal.h`/
  `threads.h` would declare an N3924 API that libc does not implement
  there. Accept the dangling declarations temporarily (glibc tolerates
  unimplemented-but-declared APIs) or wrap the mirror blocks in a
  Hurd-excluding condition too — decide during Phase 2/6; document the
  choice.
  Rationale: the reference's default `WG14_SIGNALS_GETTID` =
  `syscall(SYS_gettid)` does not compile on Hurd (no `syscall()`), and
  glibc's Hurd signal implementation is mach-message based rather than
  rt_sigaction-based — the coexistence claims in §"Comprehensive review"
  are Linux-specific.
- Deferred items to revisit after the Linux integration is proven:
  (a) a Hurd GETTID shim + assessment of mach signal semantics;
  (b) whether `siguninstall_system` should be wired to anything on
  glibc (it is part of the N3924 surface; on glibc there are no
  system-installed guards, the reference implementation defines its
  POSIX behaviour);
  (c) whether to eventually expose `current_thread_id` as a versioned
  function instead of the reference's inline+TLS-cache design (the inline
  is async-signal-safe by construction; a function would add a PLT call —
  keep the inline).

## Test plan

- **Standalone suite (unchanged)**: `test/` (40 tests since 2026-08-27:
  38 + `siginstall_sa_flags_test` + `siginstall_default_action_test`)
  keeps running against host glibc in this repo's CI (Linux legs,
  macOS arm64 native, 27/27 Windows subset under wine, musl/FreeBSD/
  Fil-C legs) — it exercises the exact `.ipp` code the submodule embeds.
- **glibc-native (new)**: the ported `glibc/wg14/tst-*` tests run against
  the fork's built libc via `make check`; they catch embedding defects
  (SA_NOCLDWAIT stripping, internal-signal rejection, TLS model, thread-
  exit cleanup through `__cxa_thread_atexit_impl`, abort-lock interplay,
  siginfo-preserving default action).
- **Pre-existing glibc tests**: must pass unchanged (contract
  preservation): `signal/tst-*`, `nptl/tst-signal*`, `nptl/tst-kill*`,
  `sysdeps/pthread/tst-raise1`, `sysdeps/pthread/tst-pthread-exit-signal`,
  `sysdeps/pthread/tst-pthread_kill-exited`,
  `sysdeps/pthread/tst-pthread-raise-blocked-self`, the `system()` tests,
  and the `debug/` backtrace tests (this tree has no dedicated
  `stdlib/tst-abort*` — abort is covered by the raise/kill tests).
- **Header hygiene**: `check-installed-headers-c/-cxx`, `check-local-
  headers`, `conform` — unchanged pass state.
- **ABI**: `check-abi` with the updated abilists; `nm -D` spot check of
  the versioned exports (no prefixed/duplicate globals).
- **Sanitizers (optional)**: ASan/UBSan builds of glibc are not standard;
  the reference's own ASan/UBSan CI legs cover the machinery.

## Risks and mitigations

1. **Recursion through public entrypoints** — analysis (§"Comprehensive
   review") shows glibc's `sigaction`/`pthread_kill`/`abort` are safe
   entrypoints for the reference's hooks (no self-recursion: sigaction and
   pthread_kill are direct-syscall thin wrappers; abort resets SIG_DFL and
   re-raises before returning). The only functional override strips
   SA_NOCLDWAIT. The Phase 1 smoke test's abort/raise round-trips verify
   this empirically.
2. **SA_NOCLDWAIT and wait() semantics** — the shim strips it from raw-
   handler installs (llvm-libc precedent, unconditional there too). It is
   **not** restorable via `siginstall_set_sa_flags_np` (the strip happens
   at the sigaction call site, below the np-setter's storage); all other
   np-set flags work. Document in NEWS/manual; pin with
   `tst-wg14-sa-flags-np` (waitpid still reaps with SIGCHLD siginstall'ed;
   `SA_ONSTACK` altstack delivery works).
3. **glibc internal signals (SIGCANCEL/SIGSETXID)** — `__sigaction`
   rejects them with EINVAL; the wg14 machinery never installs them (the
   NULL/default set excludes them via glibc's own `sigfillset`'s
   `clear_internal_signals` plus the realtime-range deletion; an explicit
   set containing them fails the whole `siginstall` with EINVAL and a full
   rollback — §"Comprehensive review" item 4). N3924's J.3
   "silently not installed" set on glibc = SIGKILL/SIGSTOP (skipped) +
   SIGCANCEL/SIGSETXID (excluded/failing); the NULL default additionally
   excludes the whole realtime range. Covered by
   `tst-wg14-install-semantics`.
4. **TLS model** — initial-exec is already glibc's libc-object default
   and the headers force the attribute; consumer-side extern declarations
   carry the attribute via the mirrored header. Risk: a consumer TU
   redeclaring `current_thread_id_cached` without the attribute would get
   global-dynamic accesses (async-signal-unsafe) — the mirrored
   declaration is the only sanctioned spelling; document it.
5. **Header mirror drift** — the `check-wg14-mirror.py` tests-special
   entry diffs the mirror against the submodule (after the small
   normalisation list in §"Public API surface"); a submodule pin bump that
   changes the API fails the check, prompting a deliberate mirror update.
6. **Submodule availability / pinning** — non-recursive clone fails with
   an actionable Makefile error; CI checks out recursively; pins bump
   deliberately; fixes to the override contract go upstream first (the
   point of the proof).
7. **Hurd** — gated out in Phase 6; the Linux integration is unaffected.
8. **`-Wundef`/`-Wwrite-strings` on vendored code** — the shim defines the
   complete set of numerically-tested macros (`WG14_SIGNALS_SOURCE`,
   `WG14_SIGNALS_HAVE__SETJMP`, `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT` +
   the two `WG14_SIGNALS_DISABLE_*`); per-object `-Wno-*` only if needed
   and documented (llvm needed `-Wno-conversion -Wno-cast-qual` for
   `verstable.h`; glibc's warning set is smaller — likely none).
9. **`sigfence`/`SIGGUARDED_FAILURE_VALUE` macro conflicts in the wg14
   TUs** — defused by the two `WG14_SIGNALS_DISABLE_*` shim defines (the
   upstream embedder-guard feature); the mirrors in glibc's signal.h must
   be layout- and spelling-identical to the reference definitions modulo
   the normalisation list (sync check).
10. **check-abi / abilist maintenance** — `make update-all-abi-libc`
    after the first build; the CI's `make check` verifies per-arch.
11. **Abort rwlock in signal-handler context** — the shim's `__sigaction`
    takes glibc's abort write lock for every SIGABRT query/set; the wg14
    default-action reset runs inside the raw handler and can contend with
    an `abort()`-in-progress holder. Analysed non-deadlocking
    (§"Comprehensive review" item 1: the holder unconditionally re-raises
    with SIG_DFL, terminating the process); the escape hatch is routing
    the handler-context reset through lock-free `__libc_sigaction`.
    Covered by `tst-wg14-abort`.

## Open questions

1. **`current_thread_id` exposure**: not in the N3924 wording (reference
   API). Exposed in `<threads.h>` under `__USE_GNU` per the llvm-libc
   precedent; revisit before any upstream glibc submission.
2. **`siguninstall_system` semantics on glibc**: no system-installed
   guards exist; the reference's POSIX behaviour stands; revisit if the
   proposal's wording changes.
3. **Raw-handler `sa_flags` default** — **RESOLVED**: keep the shim's
   SA_NOCLDWAIT strip as glibc policy (the llvm-libc fork's strip is
   unconditional too, so this is the second-generation decision, not a
   divergence). N3924 leaves the flags to the implementation; the
   standalone suite (un-stripped default) and the glibc tests (stripped)
   both pass, proving the strip is embedder-side. SA_NOCLDWAIT is
   documented as unavailable on glibc; all other np-set flags work.
4. **Hurd**: assess the mach signal backend against the reference's POSIX
   assumptions before enabling (Phase 6); until then the subdir is gated.
5. **Symbol versioning**: `GLIBC_2.44` is the next release node; if the
   fork's integration lands before the 2.44 window closes, the node name
   stands; otherwise rebase onto the then-current node.
6. **Full LLVM-toolchain consumer test**: §"Consumer validation"'s
   optional end-to-end arm (building the llvm fork's toolchain against the
   glibc fork's libc). Cheap shape-replication is already in
   `tst-wg14-chain`; decide after Phase 3 whether a full LLVM build earns
   its CI cost.

## Appendix A — Submodule manifest

- Path: `wg14_signals/` (in the glibc fork)
- URL: `https://github.com/ned14/wg14_signals.git`
- Pin: **`2217d72eb0d1266eb8e97f2b5df0e83792b73867`** (`2217d72`
  "Implement stdc_raise() with the raw_info/raw_context contract") —
  current upstream `main` HEAD and the llvm fork's final pin. Pin
  history in the llvm fork (the override-layer work): `50042da`
  (2026-08-20, initial) -> `b2059fb` (2026-08-21, embedder override
  hooks + `WG14_SIGNALS_DISABLE_*` guards) -> `d25ccd2` (2026-08-26,
  after `866d85d`'s second integration round) -> `77f4fbd` (split the old
  `stdc_raise()` into `sighandle()` plus a reserved `stdc_raise()`) ->
  `2217d72` (implemented `stdc_raise()` and added the
  `WG14_SIGNALS_RAISE_WITH_TOKEN` hook plus the constructed
  default-context path).
- `.gitmodules` entry:
  ```
  [submodule "wg14_signals"]
      path = wg14_signals
      url = https://github.com/ned14/wg14_signals.git
  ```
- Consumed (no copies): `include/wg14_signals/config.h`,
  `thrd_signal_handle.h`, `tss_async_signal_safe.h`,
  `current_thread_id.h`, `detail/impl/{thrd_signal_handle_posix,
  thrd_signal_handle_common.ipp, tss_async_signal_safe,
  current_thread_id, thread_atexit}.c.ipp` + `linked_list.h`,
  `lock_unlock.h`, `verstable.h`; `src/wg14_signals/{current_thread_id,
  tss_async_signal_safe, thrd_signal_handle_posix, thread_atexit}.c`.
  (The `thread_atexit.cpp.ipp` C++ fallback is deliberately NOT consumed —
  glibc supplies `__cxa_thread_atexit_impl`, so the C implementation's
  cxa path is selected by the shim's `WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT`
  define.)
- Fork-owned files (tracked by the fork, outside the submodule):
  `glibc/wg14/{Makefile,embedder_shim.h,Versions,Depend,tst-*.c,
  check-wg14-mirror.py}` plus the `Makeconfig` `all-subdirs` line, the
  `signal/signal.h` + `sysdeps/pthread/threads.h` mirror blocks, the
  abilist lines, the NEWS/manual updates, and the CI workflow change. No
  fork-owned copy of any upstream file exists.

## Appendix B — Host calls in the POSIX backend: glibc action

| wg14_signals call | reference site | glibc action |
|---|---|---|
| `sigaction` | `thrd_signal_handle_posix.c.ipp` (install/restore/reset, ~6 sites) | shim override: strip `SA_NOCLDWAIT`, call `__sigaction` (validates, rejects internal signals, takes the abort wrlock for SIGABRT — §"Comprehensive review" item 1) |
| `abort` | posix `.ipp` hard-abort sites (~4) | natural -> glibc `abort()` (reset-SIG_DFL-then-reraise contract met) |
| `pthread_kill(pthread_self(), ...)` | posix `.ipp` default-action self-delivery | natural -> nptl `pthread_kill` (tgkill) |
| `syscall(SYS_gettid)` | `current_thread_id.c.ipp` (Linux branch) | natural -> glibc `syscall()` (Hurd: gated, Phase 6) |
| `memcpy`/`memset`/`malloc`/`calloc`/`free` | posix + common `.ipp`, `tss_async_signal_safe.c.ipp` | natural -> glibc's own (no signal calls) |
| `sigemptyset`/`sigfillset`/`sigaddset`/`sigdelset`/`sigismember` | posix + common `.ipp` | natural -> glibc's public sigset entrypoints |
| `setjmp`/`longjmp` (`_setjmp`) | common `.ipp` | natural -> glibc's per-arch symbols (no bridge; shim defines `WG14_SIGNALS_HAVE__SETJMP` for the `_setjmp`/`_longjmp` selection, as the standalone CMake probe does) |
| `__cxa_thread_atexit` | `thread_atexit.c.ipp` (cxa path, `#ifdef WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT`) | shim defines the macro and maps the name to glibc's `__cxa_thread_atexit_impl` (GLIBC_2.18, `include/stdlib.h:165`); the `.ipp`'s own prototype of the mapped name is identical to glibc's |
| `pthread_key_create`/`pthread_once`/`pthread_setspecific`/`pthread_getspecific` | `thread_atexit.c.ipp` fallback (excluded: cxa path used) | natural, unused |
| `errno`, `NSIG`, `siginfo_t`, `ucontext_t` | all backends | natural via glibc headers (`_GNU_SOURCE` in shim) |
| `fprintf(stderr, ...)` | `WG14_SIGNALS_STDERR_PRINTF` diagnostics (the `signal_decider_create` "handler was never installed" warning, `common.ipp.ipp:748`) | natural -> glibc stdio; the warning runs only in non-signal context and deliberately **outside** the state spinlock (the reference's own comment at `:744` cites analysis.md SDCF/SPIN — fprintf is slow and could trigger a signal delivery under the lock), so glibc's stdio locks are never taken from a handler |
