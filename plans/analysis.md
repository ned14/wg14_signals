# Exhaustive implementation analysis: wg14_signals

Scope: all headers and sources, both backends (POSIX/Windows), the header-only
configuration, both TLS paths, all error paths, and all build configurations exercised
and *not* exercised by CI; reviewed against the vendored N3924 rev 4 wording
(`docs/proposed-wording.md`).

Findings are listed in priority order (rationale in §4), fixable first, adjudicated
wontfix last. Each finding's identity is its unique four-letter code (e.g. `SPIN`), used
in citations here, in `plans/ideas.md`, and in source comments. **[confirmed]** =
reproduced on macOS (arm64, clang 17, ASan/UBSan where noted); **[probe-verified]** =
verified against the current tree with throwaway probes; **[wontfix]** = adjudicated
not-to-fix, rationale in the heading body. Windows-only items are code-level findings
(no Windows host) verified against the MSVC build matrix in CI. Merged findings cite the
earlier finding rather than duplicating it (e.g. `DEIN` extends `UNTL`).

Removed findings (fixed or refuted; fix details live in source comments and regression
tests): 2026-08-17 `JLGS` (dangling-frame; cooperative case fixed, uncooperative
wontfix), `SJMP` (setjmp-buffer race), `NSTR` (nested-delivery `rsi` race); 2026-08-18
`NDEC` (frame-pinning decider), `SDDF` (out-of-lock decider-node free), `PFXM`
(`WG14_SIGNALS_PREFIX` misuse refuted), `VDED` (Windows V5 dedup cache), `GLIN`
(verstable-map init race), `DFLT` (default re-raise restores installed handler), `UNKN`
(unknown-signal fallback no longer resets to `SIG_DFL`), `TIDR` (thread-id reuse), `SDCF`
(`signal_decider_destroy` return contract), `UCLK` (destroy re-entrancy), `NEGS`
(out-of-range signo rejection), `VSDT` (sigset helper return type), `NSIH` (`SA_SIGINFO`
hand-off synthesises `siginfo_t`), `ENUM`/`TYDF` (identifier renames), `WFRM` (Windows
dispatch is now frame-first, N3924-compliant). Also on 2026-08-18, duplicate findings
were merged into their primaries (which absorb them): `ZERO`->`IGND`, `REEN`->`UCRE`,
`CXAT`->`TAFL`, `LEAK`->`SPIN`, `WRET`->`PREI`, `SJMS`->`SJSP`, `MSQR`->`MUTR`,
`DEDE`->`TSSD`.

On 2026-08-19 a fresh exhaustive comparison against the merged rev-5 wording
(`docs/proposed-wording.md`, HEAD `ef43686`) added seven findings at the top of §1 —
`RFLK`, `RAIS`, `HNDF`, `SIGM`, `NRAI`, `WVLD`, `ABRS` — and amended `ACTV` (Windows
frame filter is not activation-gated), `IGND` (facet 2's Windows arm changed by the V5
dispatch rework), `SKIP` (the skip is now conforming; the reverse NULL-on-failure arm
is the deviation), `TSSG` (destroy half resolved by rev-5 7.30.6.6 p3), `GDIR`
(rev-5 p11 defines global `call_recovery`), `SPIN` (destroy-in-decider is not
async-signal-safe), `SFNK` (rev-5 5.2.2.4 p7 guarantee rests on a QoI assumption on the
fallback), and the §3 minor-conformance notes.

On 2026-08-20 fixed: `MUSL` (musl `siginfo_t`/`ucontext_t` dispatch; musl CI leg added),
`SIGN` (`SIGSYS`/`SIGXCPU`/`SIGXFSZ` guards), `RTIM` (`siginstall()` keeps
`sigfillset()` for the NULL case but filters `SIGCANCEL`/`SIGSETXID` out of **all**
guarded inputs, and the realtime range out of the NULL case), `TOPL`
(`CMAKE_SOURCE_DIR`/`CMAKE_CURRENT_SOURCE_DIR` comparison), `CSTD` (PUBLIC
`c_std_11`/`cxx_std_11` compile features propagate to consumers), `PCFG` (config no
longer references phantom exports), `THRD` (`thrd_join`/`thrd_create` shim fixes),
`TCOV` (`edge_api_coverage_test`), `DECR` (`signal_decider_create` doc describes the
enum contract). Also on 2026-08-20, the first successful Windows CI test run exposed a
latent defect in the V5 dedup / raise-initiated detection: `win32_exception_record_matches`
compared the first `ExceptionInformation` parameter only when `NumberParameters > 0`, which
made every 0-parameter exception -- `stdc_raise(signo, NULL, NULL)` and
`RaiseException(code, 0, 0, NULL)`, the common paths -- fail to match. That re-ran the
global deciders on the vectored-continue follow-up (`sigguarded_tss_init_test` saw
`global_decider_called == 2`), and failed to mark 0-parameter unclaimed software raises
(`stdc_raise_uninstalled_test` and `out_of_range_signo_test` died with the raised
exception). The predicate now treats `NumberParameters == 0` as a match on
code/flags/count alone; `out_of_range_signo_test` also switched its MSVC
`GUARDED_SIGNAL` fallback from `SIGABRT` to `SIGILL` (SIGABRT maps to the
non-continuable `EXCEPTION_NONCONTINUABLE_EXCEPTION`, which a resume-returning frame
decider cannot continue -- the `SABA` loop). The unclaimed-raise path was further
hardened: the map-entry branch now treats a software raise with no claiming decider
as unclaimed (the PREI fix direction) instead of returning `CONTINUE_SEARCH` to WER.
`out_of_range_signo_test` now calls `siginstall()` so the raise-initiated machinery
exists on Windows (the unclaimed-raise path requires the library's vectored/unhandled
handlers, which only `siginstall()` registers).

The remaining two Windows CI failures (`out_of_range_signo_test`, `stdc_raise_uninstalled_test`)
were then diagnosed by cross-compiling the real backend with clang 24
(`--target=x86_64-w64-windows-gnu -fms-extensions`) and running the actual tests under
wine, and by diffing against the last passing commit (b6657fc8, whose no-entry branch
used a `software_raise_in_progress` flag with no record comparison). Three defects were
found and fixed in `thrd_signal_handle_windows.c.ipp`:

1. **The unclaimed and claim-marking checks now use the full record predicate
   (`win32_exception_record_matches`) everywhere** -- the raise-initiated frame vs the
   delivered `EXCEPTION_RECORD`. The NULL-info raise path snapshots
   `ExceptionFlags=0`/`NumberParameters=0` and `RaiseException(code, 0, 0, NULL)` delivers
   exactly that (matched via the `NumberParameters == 0` short-circuit); the info path
   snapshots all four fields from the exact values passed, which the OS preserves. A
   code-only comparison would be a false-positive hazard: a genuine fault of the same
   code raised during an in-flight raise (e.g. `stdc_raise(SIGSEGV, NULL, NULL)` whose
   decider faults with a real `0xC0000005` access violation carrying
   `NumberParameters == 2`) would be mistaken for the software raise and re-executed
   forever via `CONTINUE_EXECUTION`.
2. **Frame claims record the V5 dedup decision.** A frame filter returning
   `EXCEPTION_CONTINUE_EXECUTION` terminates the dispatch with execution resumed, but the
   vectored continue handler is still invoked on that path (ReactOS
   `sdk/lib/rtl/i386/except.c` and `amd64/except.c`, matching Windows x64; the VCH result
   is ignored there -- `vectoreh.c`, "execution always continues"). The frame filter
   records its claim (CONTINUE_EXECUTION) in the dedup so that continue-handler
   invocation reuses it instead of re-running the global pass; the pass re-run would see
   the in-flight software raise and, without the marker, report it unclaimed
   (`stdc_raise()` returning false for a raise a frame decider just claimed --
   `out_of_range_signo_test`). The in-flight raise frame also gains an
   `exception_was_claimed` marker (full-record match) so a pass re-run (e.g. a dedup
   mismatch) still continues rather than reporting the raise unclaimed.
3. **The no-entry unclaimed branch records its decision too**, so the continue handler
   reuses it instead of re-running the pass.

A ReactOS cross-check (`dll/win32/kernel32/client/except.c`) then found one further
defect in the info-path snapshot: **`kernel32!RaiseException` masks the delivered flags to
`EXCEPTION_NONCONTINUABLE` only**, but `stdc_raise`'s info path snapshotted
`info->ExceptionFlags` verbatim into the raise-initiated frame and passed the unmasked
value to `RaiseException`. A caller passing any stray flag bit made the snapshot-vs-record
comparison fail, so the raise-initiated detection missed and WER terminated the process.
The snapshot and the `RaiseException` call now both use
`info->ExceptionFlags & EXCEPTION_NONCONTINUABLE` (ReactOS-verified masking).

The `__MINGW32__` sigguarded guard now allows clang (`__clang__`), which implements
`__try`/`__except` on Windows targets -- the previous unconditional `#error` also
blocked clang-based Windows builds.

On 2026-08-21 the two tests fixed above (`out_of_range_signo_test`,
`stdc_raise_uninstalled_test`) were found to still die with the raised exception
code on **real** Windows (VS2022 x64 CI and a Windows ARM64 UTM VM), despite
passing under the wine cross-check used to verify the 2026-08-20 fixes. Root
cause: **real Windows ORs `EXCEPTION_SOFTWARE_ORIGINATE` (0x80) into
`ExceptionFlags` of every record delivered by `RaiseException()`**, but
`stdc_raise()`'s raise-initiated snapshot records only the flags it passed (0 on
the NULL-info path, `info->ExceptionFlags & EXCEPTION_NONCONTINUABLE` on the
info path), and the wine/ReactOS delivery model adds no such bit. The strict
`state->ExceptionFlags == record->ExceptionFlags` comparison in
`win32_exception_record_matches` therefore never matched on real Windows, the
in-flight raise frame was never found, the unclaimed-raise machinery returned
`CONTINUE_SEARCH`, and Windows Error Reporting terminated the process. All 37
Windows tests pass on real Windows with the fix.

Later the same day, simplifying the raise detection down to the
`EXCEPTION_SOFTWARE_ORIGINATE` bit was attempted and **reverted** after
empirical testing showed the bit cannot be relied on or forced:

1. **The bit cannot be forced on for cross-platform consistency.** It was
   proposed to pass `EXCEPTION_SOFTWARE_ORIGINATE` as the `RaiseException` flags
   argument so every platform delivers it. Testing proved this impossible: both
   kernel32 (real Windows; ReactOS `kernel32/client/except.c` masks the argument
   to `EXCEPTION_NONCONTINUABLE`) and wine mask the argument to
   `EXCEPTION_NONCONTINUABLE`, and the delivered 0x80 on real Windows comes from
   ntdll's dispatcher, which adds it regardless of the argument. Under wine,
   `RaiseException(code, EXCEPTION_SOFTWARE_ORIGINATE, ...)` delivers
   `ExceptionFlags == 0` and `RaiseException(code, 0x81, ...)` delivers `0x01`.
   The delivered flags are therefore OS-controlled, not caller-controlled.
2. **The bit alone cannot discriminate our raises.** Replacing the four-field
   comparison with "0x80 set + code matches" breaks the wine verification path
   (wine never sets the bit, so every in-flight raise would be missed and the
   unclaimed-raise machinery would return `CONTINUE_SEARCH` again) and
   false-positives on real Windows (a user's own `RaiseException` of the same
   code during our dispatch also carries the bit, and only the parameter fields
   tell it apart).
3. **The correct approach remains the masked comparison.** The raise frame keeps
   its pre-delivery snapshot of all four identity fields
   (`ExceptionCode`, `ExceptionFlags` masked to `EXCEPTION_NONCONTINUABLE`,
   `NumberParameters`, `ExceptionInformationFirst`), and
   `win32_exception_record_matches` masks `EXCEPTION_SOFTWARE_ORIGINATE` out of
   both sides, making the comparison agnostic to whether the OS marks
   software-originated exceptions. Both environments verified: the two failing
   tests pass under wine (bit absent) and all 37 tests pass on real Windows
   (bit present).

On 2026-08-21 the **no-install-at-all arm of W5 was closed**: `stdc_raise()`
of a signal with *no* `siginstall()` ever performed previously terminated the
process on Windows -- the exception resolution machinery (vectored continue
handler + unhandled exception filter) is registered only by
`install_sighandler_impl()`, so an uninstalled raise reached Windows Error
Reporting and `stdc_raise()` never returned (POSIX returned false via the map
miss). `stdc_raise()` now pre-checks the signo-to-sighandler map under the
state lock and returns false before raising anything when no handler is
installed, so the raise cannot happen without the machinery present and the
non-continuable SIGABRT case (whose exception cannot be resolved even by the
machinery) also returns false. This is exact POSIX parity: the map miss is the
documented "no decider installed for that signal" return, and the concurrency
properties are unchanged (the pre-check and the raise are the same
check-then-act window a concurrent `siguninstall()` already races with,
analysis.md 2.2/W4). The new `stdc_raise_noinstall_test` covers the
no-install raise as the very first library call, inside a `sigguarded()`
frame, and on Windows.

---

## 1. Findings, in priority order

### Rev-5 wording conformance review (2026-08-19)

A fresh exhaustive re-comparison of the implementation against the current
`docs/proposed-wording.md` (rev 5, merged at HEAD 2026-08-19) found the
following new deviations, all of which post-date or were missed by the
rev-4-based findings below. The rev-5 wording differs materially from rev 4 in
four places that invalidate or weaken earlier findings: 7.14.1 p14 (default
handling when the ordered sequence is empty; the `signal`-function handler is
*not used* while activated), 7.14.2.9 p3/p4 (`stdc_raise` behaves as-if
`raise` when not activated), 7.14.2.5 p4 (silently-not-installed signals
report success), and 7.30.6.6 p3 (destroy-while-threads-live is now explicit
UB). Each is marked with its affected wording paragraph.

### `RAIS` [wontfix] [semantics, both backends, Med] `stdc_raise` of a non-activated signal does not behave as-if `raise()` (7.14.2.9 p3, p4)

**Adjudicated wontfix 2026-08-20:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record. The as-if-raise fallback is a wording-vs-documented-contract conflict: the header documents the narrower behaviour ("returning false if we have no decider installed for that signal", `thrd_signal_handle.h:622-626`), and implementing the wording would change that documented return contract on both backends.

The wording: "This function behaves as if it called the `raise` function
(7.14.2.4) with the argument `signo`" and "If `signo` is not activated, this
function behaves as if it called the `raise` function and no signal deciders
are invoked." The implementation never performs the as-if `raise`: POSIX
returns `false` with no delivery (`thrd_signal_handle_posix.c.ipp:417-428`),
Windows raises an SEH exception that round-trips to `false` when the library's
vectored handler is installed (`thrd_signal_handle_windows.c.ipp:389-427`) and
reaches Windows Error Reporting when it is not (finding `PREI`). Consequences
on both backends: the `signal`-function handler is never called, and the
default action never occurs — `stdc_raise(SIGTERM)` on a fresh process
returns `false` instead of terminating the process as `raise(SIGTERM)` would.
(7.14.2.3 p4 as amended — "If the signal occurs as the result of calling the
`abort`, `raise`, or `stdc_raise` function, the signal handler shall not call
the `raise` or `stdc_raise` function" — confirms the wording intends the
handler to be callable as a result of `stdc_raise`.) The header documents the
implementation's narrower contract ("returning false if we have no decider
installed for that signal", `thrd_signal_handle.h:622-626`); the wording
deviates from it. Related but distinct from `IGND` (return-value contract when
deciders *were* consulted) and `ACTV` (deciders consulted without activation).

### `HNDF` [wontfix] [semantics, both backends, Med] activated-signal hand-off to the previously installed handler where the wording requires default handling (7.14.1 p14)

**Adjudicated wontfix 2026-08-20:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record. Chaining to the previously installed handler is the library's documented design (`thrd_signal_handle.h:628-640`); aligning with 7.14.1 p14 would abandon that design (reset to `SIG_DFL` and re-deliver), and the fix would need a wording decision.

The wording: "If every signal decider in the ordered sequence returns
`sig_decision_next_decider`, or if the ordered sequence is empty, the default
handling for that signal number on that implementation is performed. While a
signal number is activated, the signal handler installed by the `signal`
function for that signal number is not used to handle signals with that
number." The POSIX backend, when no decider claims, calls
`invoke_sigaction(&sa)` with `sa` = the handler that predated `siginstall`
(`thrd_signal_handle_posix.c.ipp:474-479`): an application handler installed
via `signal()`/`sigaction` before `siginstall` *is* invoked while the signal is
activated — the exact behaviour p14 forbids — and the default handling never
runs. (Only when the old handler is `SIG_DFL`/`SIG_IGN` does the behaviour
coincide with the wording.) Windows has no chaining at all: the unclaimed
raise terminates via WER (`PREI`), and CRT `signal()`-installed handlers never
run (`CRTS`). The chaining is the library's documented design
(`thrd_signal_handle.h:628-640`); the rev-5 wording contradicts it. Note the
interaction with `RAIS`: for an activated signal with zero deciders the POSIX
hand-off calls the `signal`-function handler, so both p14 sentences are
violated by the same path. Either the wording should explicitly permit
chaining to the previously installed handler as the "default handling" (it
does not say this), or the implementation should reset to `SIG_DFL` and
re-deliver.

### `NRAI` [code-level, Windows, Low] `stdc_raise` of an invalid signo raises an exception instead of "returns false without raising a signal" (7.14.2.9 p6)

The wording: "If `signo` is not a valid signal number (7.14.1), this function
returns false without raising a signal." POSIX conforms (bounds-rejects before
any action, `thrd_signal_handle_posix.c.ipp:357-371`). Windows maps any
non-zero signo through `win32_exception_code_from_signal`
(`thrd_signal_handle_windows.c.ipp:389-418`): an invalid signo — including a
negative one — becomes `0x40000000 | (signo & 0x3FFFFFFF)` and is actually
raised via `RaiseException`. With the library's vectored handler installed the
exception round-trips to `false` (invisible unless a debugger is attached, in
which case the continue handler runs the pass), but without a `siginstall` the
raise reaches WER and terminates the process (`PREI`). The p6 guarantee
"without raising" does not hold on Windows in either configuration.

### `WVLD` [code-level, Windows, Low] Windows sigset helpers accept signal numbers 23..32 that are not valid signal numbers (7.14.2.2)

MSVC defines signals 1..22 (`NSIG == 23`), and the header adds `SIGBUS`(7),
`SIGKILL`(9), `SIGSTOP`(19) — all within 1..22. The Windows helpers
(`thrd_signal_handle.h:71-99`) accept 1..32: `sigaddset`/`sigdelset` set the
bit and return 0, and `sigismember` returns 1, for signo 23..32, which are
not signal numbers of signals defined by the implementation. The wording
requires "If `signo` is not a valid signal number (7.14.1), the set is not
modified" and "returns zero if `signo` is a valid signal number and a negative
value otherwise". Likewise `sigfillset` (`= UINT32_MAX`) includes bits for the
undefined signals 23..32 where the wording defines the full set as "the set of
all signals defined by the implementation". (`siginstall`'s loop bounds at
`NSIG == 23` never installs them, so the damage is confined to the helpers.)

### `ABRS` [wontfix] [code-level, both backends, Low] `sigdecider_abandon_resume` aborts when a nested signal's processing changed `tss->front` between the abandon and the resume — in a call sequence the wording permits

**Adjudicated wontfix 2026-08-20:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record. Niche wording-valid sequence (nested delivery inside the abandon/resume window) that aborts rather than corrupts memory; tracking the intervening `tss->front` rewrites would require changing the abandon machinery for a trigger the document itself rates Low.

A thread-local decider calls `sigdecider_abandon` (frame popped, `tss->front`
= the frame below), a nested signal is delivered before the resume (possible
via `SA_NODEFER`), and the nested raise's processing claims via a recovery
`longjmp` into an outer frame, rewriting `tss->front`. The decider then calls
`sigdecider_abandon_resume` with the same `rsi` in the same decider call —
every condition of 7.14.3.3 is satisfied — and the implementation's topmost
check (`tss->front != rsi->internal_local_decider->prev`) fails
(`thrd_signal_handle_posix.c.ipp:531-536`, Windows `:504-509`), aborting the
process. The abandonment state is a plain bool on the `rsi`; the machinery
cannot tell that the intervening rewrite was legitimate. Niche (needs the
nested delivery inside the abandon/resume window) but a genuine wording-valid
call that terminates the program.

### `IGND` [code-level, both backends, Low] `stdc_raise` returns `true` even when the raise was silently ignored (SIG_IGN/default-ignore hand-off, or zero deciders called)

**Update (2026-08-20):** the Windows map-entry arm of facet 2 changed with the PREI
fix (the map-entry branch now marks an unclaimed software raise `exception_was_unclaimed`
and `stdc_raise()` returns `false`, `thrd_signal_handle_windows.c.ipp:745-779`). So on
Windows the *zero-decider* case now returns `false` (conforming: N3924 requires `true`
only when at least one decider was called), while the *deciders-called-but-unclaimed*
case also returns `false` — still a deviation, which should hand off and return `true`
per the "decider was called" criterion. The POSIX facet-1/facet-2 arms below are
unchanged.

Two facets of the same `stdc_raise` return-value contract defect, merged 2026-08-18
(from the former `ZERO`):

1. **SIG_IGN / default-ignore hand-off (POSIX).** `thrd_signal_handle_posix.c.ipp:394-397` —
   when the map has an entry for the signal but no global decider claims it, `stdc_raise`
   calls `invoke_sigaction(&sa, ...)` and unconditionally returns `true`. If the
   pre-library handler was `SIG_IGN` (or the default action is ignore —
   SIGCHLD/SIGURG/SIGWINCH), `invoke_sigaction` returns `false` but `stdc_raise` still
   returns `true`, violating the documented contract. Callers using the documented
   `if(!stdc_raise(...)) { fall back }` idiom will not detect the silently-ignored case.
   **Probe-verified (macOS arm64):** `stdc_raise(SIGUSR1, NULL, NULL)` with SIGUSR1
   installed and a pre-install `SIG_IGN` disposition returned `true` (the signal was
   silently ignored); with a benign pre-install handler it returned `true` and the previous
   handler ran once.

2. **Zero deciders called (both backends).** N3924 (7.14.2.9): "returns true if at least
   one signal decider installed under this facility was called." With a map entry for the
   signal but an empty decider list, the POSIX backend hands off to the previously
   installed handler and returns `true`:
   `thrd_signal_handle_posix.c.ipp:474-479` (`invoke_sigaction` then unconditional
   `return true`). The Windows facet of this half changed with the V5 dispatch rework: a
   `stdc_raise`-initiated raise of an installed signal with no claiming decider now
   returns `CONTINUE_SEARCH` from the pass and terminates via WER — `stdc_raise` never
   returns (finding `PREI`, facet 2) — so the "returns `true`" Windows arm of this
   finding survives only in the user's-own-`__except` case (`unclaimed == 0`, so
   `stdc_raise` returns `true`). Probe
   (`stdc_raise(SIGUSR1, NULL, NULL)` with SIGUSR1 installed and zero deciders, previous
   handler installed via `signal()`): returned `true`, old handler called. The proposal's
   criterion is "decider called", not "decider claimed", so the case where deciders were
   called but all returned `next_decider` (then the hand-off runs and the implementation
   returns `true`) *is* conforming; only the zero-decider case deviates. Facet 1's
   scenario is likewise conforming to the proposal when a decider was called — both facets
   are deviations from the header's own "returning false if we have no decider installed
   for that signal" contract (`thrd_signal_handle.h:593-626`), which the header does not
   qualify. Re-verified 2026-08-19: unchanged by the rev-5 wording; the zero-decider
   `true` additionally conflicts with the rev-5 p14 default-handling rule — see finding
   `HNDF`.

### `ACTV` [semantics, both backends, Low] deciders are consulted without activation, contrary to the rev-5 wording's activation gate

Logged 2026-08-19 against the rev-5 wording. The activation model (7.14.1:
decider invocation only while the signal number is activated; "no signal
deciders are invoked for it" while not activated) is stated in `stdc_raise`
itself: "Signal deciders are invoked for `signo` only if `signo` is activated.
If `signo` is not activated, this function behaves as if it called the `raise`
function and no signal deciders are invoked." The POSIX
backend walks the per-thread `sigguarded` frames **before** consulting the activation
map (`thrd_signal_handle_posix.c.ipp:372-415`): a frame guarding a never-installed (or
fully-uninstalled) signal has its decider invoked, may claim the raise (including via a
recovery longjmp), and makes `stdc_raise` return `true` — the wording's "as if `raise`,
no deciders" fallback never runs in that case. Real-signal delivery on POSIX is
conformant (the library's handler exists only while installed); only the simulated-raise
path deviates. The Windows backend is **not** conformant either — the rev-4-era claim
below is wrong (re-verified 2026-08-19): `win32_exception_filter`
(`thrd_signal_handle_windows.c.ipp:268-297`) runs the frame decider whenever
`sigismember(guarded, signo)` is truthy, with no map/activation check, for both genuine
faults on never-installed signals and `stdc_raise` of never-installed signals — and a
frame claim there used to leak the raise frame (finding `RFLK`, fixed: the
filter's `EXECUTE_HANDLER` path pops the per-thread raise chain). Only the Windows *global*
pass is activation-gated (map lookup, `:585-606`). Both backends therefore violate
7.14.1 p15's "no signal deciders are invoked" for non-activated signals.
Interaction with `IGND`: a decider invoked for a non-activated signal satisfies the
"returns true if at least one … decider … was called" criterion, so the ill-gotten
invocation also legitimises an otherwise-zero-decider `true` return.

Fix: gate the POSIX frame walk on the map (as the Windows global pass does), and gate
the Windows frame filter on the map as well; or, if the activation gate is softened,
align `stdc_raise`'s description accordingly.

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
   thread exit. (The leaked entry can no longer be *observed* by a reused TID — the
   `TIDR` generation fix gives a fresh thread its own key — but the entry and state
   still leak, and they are only reclaimed by a later `destroy`.)
2. `sig_global_tss_state_init`'s `thread_atexit(free, mem)`
   (`thrd_signal_handle_common.ipp.ipp:296-317`) never frees the per-thread raise state.

CI never compiles the branch: every CI platform's probe finds `__cxa_thread_atexit` (and
the library build prefers `thread_atexit.cpp` when `__cxa` is absent), so only header-only
C consumers without the define, or exotic non-`__cxa` POSIX platforms, hit it. Fix: store
the list head in the key value itself (the destructor's argument survives) and/or probe
and reject the fallback on affected platforms at configure time.

### `CPPR` Static library requires a C++ runtime, not declared [open on non-`__cxa_thread_atexit()` platforms]

On platforms without `__cxa_thread_atexit()` (e.g. Windows), `thread_atexit.cpp` is
compiled into the library and the CMake package does not express the C++ standard library
dependency, so a plain C consumer linking `libwg14_signals.a` there gets unresolved C++
runtime symbols. (On `__cxa_thread_atexit()` platforms the C file is compiled instead and
the library is all-C with no C++ runtime dependency.)

### `STOR` Test storage exhaustion

`test/async_signal_safe_tls_test.c:6-14` and `test/header_only_test.cpp:16-24`: `create`
does `*dest = storage_ptr++` on a 2-element array; any third `thread_init` (e.g., a
re-run of the main-thread init after the worker also inits, or the documented "safe to
call many times") writes out of bounds. **Extended:** the same applies to
`test/benchmark_async_signal_safe_tls_test.c:12-13`. The test also never verifies the
re-init and re-entrancy semantics documented in the API.

### `SABA` [code-level, Windows, Low] `stdc_raise(SIGABRT)` raises a non-continuable exception; "resume" from a decider loops

`SIGABRT` maps to `EXCEPTION_NONCONTINUABLE_EXCEPTION (0xC0000025)`. If any decider
returns "resume execution" for it, Windows re-raises `0xC0000025` (the OS cannot resume a
non-continuable exception), the vectored handler runs the decider again -> repeat loop.
POSIX has no such constraint for SIGABRT. (With no decider and an enclosing `__except` the
raise still works, which is why tests pass.) Since the dispatch rework the loop survives
only for genuine/app-raised non-continuable exceptions with no `tss->front` to longjmp
into, or through the frame filter's resume; a `stdc_raise(SIGABRT)` claimed by a global
decider longjmps into the raise frame instead of looping.

### `MUTR` [code-level, Windows, Low] `stdc_raise`'s `EXCEPTION_RECORD` parameter protocol mutates the caller's record, surfaces user parameters as `addr`/`error_code`, and drops `raw_context` on a full record

Three facets of the raise-time `ExceptionInformation[]` protocol, merged 2026-08-18 (from
the former `MSQR` and the §3 raw_context note):

1. **Caller's record is mutated.** `thrd_signal_handle_windows.c.ipp:396-414`: when
   `info != NULL` and room remains, the function appends the `0xdeadbeefdeadbeef` marker
   and the raw context into `info->ExceptionInformation[]` and bumps `NumberParameters` —
   mutating the caller's record in place. If the caller passes a kernel-supplied
   `EXCEPTION_RECORD` (re-raising a genuine fault from inside a filter — exactly the
   "pass on signal handling to this library" use case documented in the header), the
   record the kernel will later inspect is altered. The marker write is also racy if two
   threads re-raise through the same record.
2. **User parameters masquerade as `addr`/`error_code`.** The dispatch reads
   `ExceptionInformation[1]` and `[2]` as `addr` and `error_code` (now behind
   `NumberParameters >= 3` / `>= 2` guards, `:256-265`, but the substance persists). For a
   user raise via `stdc_raise(signo, info, ctx)` with >= 2 user parameters the array holds
   the *caller's own parameters*, so deciders see arbitrary user data in `addr`/`error_code`
   (deterministic for non-NULL `info`, not garbage). Any decider keying on the NTSTATUS in
   `error_code` gets different values for user raises than for genuine faults.
3. **Full record drops `raw_context`.** When `NumberParameters + 2 >
   EXCEPTION_MAXIMUM_PARAMETERS` the marker append is skipped (`:398-400`) and the
   decider's `raw_context` silently becomes the raise-time `ContextRecord` instead of the
   caller's context.

**Wording conformance, re-verified 2026-08-19** against the rev-5 wording: 7.14.2.9 now
adds "If `raw_info` is a null pointer, the `raw_info` member is a null pointer, the
`error_code` member is zero, and the `addr` member is a null pointer." Windows cannot
conform: `raw_info` is always the (possibly synthesized) `EXCEPTION_RECORD`, never null,
and `error_code`/`addr` come from the record's parameters, not zero. POSIX conforms.

### `UFLT` [code-level, Windows, Low] `siguninstall` clobbers an application-installed `SetUnhandledExceptionFilter`

`thrd_signal_handle_windows.c.ipp:778-792`: the library captures the
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
a concurrency caveat and does not cover the sequential case. Also, `install_sighandler_impl`
(`:778-779`) *unconditionally* replaces the process filter at install time even if one is
already present (restoring the pre-library one only at the final `siguninstall`), so an app
that installs its filter after the first `siginstall` loses it even while other signals
remain installed — the former §3 note, merged 2026-08-18. (POSIX has no analogue:
`sigaction` chaining preserves the old handler.)

### `GDIR` [code-level, both backends, Low] a *global* decider returning `sig_decision_call_recovery` has divergent, undocumented semantics

The enum documentation (`thrd_signal_handle.h:254-257`) says `sig_decision_call_recovery`
is "Thread local signal deciders only", yet global deciders share the same `sig_decide_t`
type and both backends accept it. **Rev-5 wording (2026-08-19):** 7.14.1 p11 now
explicitly defines the global-decider contract for this value — "`sig_decision_call_recovery`: the default handling for that signal number on that implementation is performed" — so both backends deviate from the wording, not merely from the header. **POSIX** (`thrd_signal_handle_posix.c.ipp:466-471`):
`if(res)` treats *any* non-zero decision as "claim and `return true`" — for
`call_recovery` the raise is claimed, no recovery is ever called, and for a genuine
fault the handler returns and the faulting instruction re-executes (an infinite re-fault
livelock),
**even when a guarding `sigguarded` frame exists**. **Windows**
(`thrd_signal_handle_windows.c.ipp:645-676`): the same value causes a
`longjmp(tss->front->buf, 1)` into the top frame when one exists (guarded by
`tss != NULL && tss->front != NULL`, `:659-660`, so the former NULL-deref facet is
gone), or records the decision and returns `EXCEPTION_CONTINUE_EXECUTION`
(`:667-675`) otherwise — generally ending the process. So one enum value produces "claim,
no recovery, re-fault" on POSIX and "unwind to top frame" on Windows, where the wording
requires default handling on both. Neither backend
documents or diagnoses this for global deciders.

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

### `UCRE` [code-level, Low] `thread_init`'s unlocked `attr.create` breaks the THREADSAFE contract (concurrent first-use race; signal-re-entrancy double-create leak)

Two facets of the same unlocked `create` window in `tss_async_signal_safe_thread_init`
(`tss_async_signal_safe.c.ipp:292-309`: unlock, call `mem->attr.create`, re-lock before
insert), merged 2026-08-18 (from the former `REEN`):

1. **Cross-thread race.** Two threads racing to first-initialise the same handle call the
   user's `create` concurrently. The API documents `thread_init` as THREADSAFE and
   serialises the map insert but not the create callback. The test suite's own `create`
   uses a shared `static unsigned *storage_ptr`, so a two-worker concurrent first-use would
   be a data race in the harness itself (cf. `STOR`).
2. **Same-thread signal re-entrancy.** If a signal handler runs `thread_init` on the same
   object in the unlocked window (possible via `stdc_raise` -> `sig_global_tss_state_init`
   -> `thread_init`), the user's `create` callback runs twice and the second `insert`
   replaces the first entry: the first value leaks (no destructor on replace), `count` is
   double-incremented, and two atexit registrations are queued.

Design note: resolved 2026-08-19 in the "document" direction — the N3924 rev 5 wording
(7.30.1, `tss_async_signal_safe_attr`) and the header
(`tss_async_signal_safe.h:37-38`) now require the `create` and `destroy` functions to be
thread-safe and reentrant, so the unlocked call window is a user contract rather than an
implementation defect; the double-create leak in facet 2 remains an implementation issue
for user functions that do not satisfy the contract.

### `TSSG` [docs/API contract, both backends, Low] `tss_async_signal_safe_get`'s header omits the wording's precondition (destroy half resolved by rev-5)

**Rev-5 amendment (2026-08-19):** the destroy half is resolved — rev-5
7.30.6.6 p3 now states exactly what the implementation's header requires: "It
is undefined behavior if this function is called while any thread that has
called the `tss_async_signal_safe_thread_init` function for this instance has
not yet exited." The implementation's documented precondition
(`tss_async_signal_safe.h:46-52`) therefore matches the wording, and the
"stricter than the wording" half of this finding no longer applies (the
implementation's inability to destroy live threads' pointers safely — the
`DEIN` race — is now inside the wording's UB). The `get` half remains open,
re-verified below.

The wording requires `tss_async_signal_safe_get()`: `tss_async_signal_safe_thread_init()`
"shall have been called on the same thread for the instance identified by `val`... otherwise the behavior is
undefined." The implementation returns NULL for an uninitialised thread
(`tss_async_signal_safe.c.ipp:273-290`) — a defined, benignly stronger behaviour — but the
header (`tss_async_signal_safe.h:68-72`) documents neither the precondition nor the NULL
fallback, so the "ASYNC-SIGNAL-SAFE" claim is asserted for a function whose first call per
thread performs the `my_current_thread_id()` cache fill (`MAST`) and a spinlocked map lookup
(`SPIN`) — see §2. The NULL extension (defining formerly-undefined
behaviour) remains conforming.

### `MLAS` [wontfix] Modified-local-after-setjmp UB in POSIX `sigguarded`
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_posix.c.ipp:282-326`: `current.rsi` is written by `prepare_rsi` (via
the frame pointer in the signal handler, i.e. after `setjmp` executed) and then read after
`longjmp`. Per C11 7.13.2.1p3, non-volatile automatic objects modified between `setjmp`
and `longjmp` have indeterminate values after `longjmp` — this is UB (works in practice on
mainstream compilers because the frame is a memory object, but a conforming compiler may
cache `current` in registers). The struct should be `volatile` (or the members accessed
post-longjmp should be). Re-verified 2026-08-19 against the rev-5 wording: the
non-local-jump paragraphs (5.2.2.4 p9/p10) codify the same indeterminate-representation
rule for non-volatile automatic objects changed between save and jump; the finding is
unchanged (the frame's `rsi` is read by recovery after the jump, and 5.2.2.4 p10's
sigfence exception does not apply).

### `PREI` [wontfix] [Windows, Med] an unclaimed `stdc_raise` (before any `siginstall`, or of an installed signal with no decider/frame/user `__except`) terminates the process via Windows Error Reporting; POSIX returns `false` / hands off in-process

**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

**Update (2026-08-20):** facet 2's fix direction was implemented. The map-entry branch
of `win32_global_decider_pass` now checks `stdc_raise_initiated_exception` after the
decider loop and, for a matching software raise with no claiming decider, sets
`exception_was_unclaimed = true` and returns `EXCEPTION_CONTINUE_EXECUTION` (recording
the decision for the V5 dedup), so `stdc_raise()` returns `false` instead of the raise
reaching WER (`thrd_signal_handle_windows.c.ipp:745-779`). **Facet 1 remains the unfixed
wontfix remainder:** before any `siginstall` the unclaimed-raise machinery
(`SetUnhandledExceptionFilter`/`AddVectoredContinueHandler`, registered only by
`install_sighandler_impl`) does not exist, so a `stdc_raise()` still reaches WER.

Two scenarios of the same "unclaimed raise reaches WER" mechanism, merged 2026-08-18
(from the former `WRET`). On POSIX, `stdc_raise` for an uninstalled signal runs the
chain in-process and returns `false` (`thrd_signal_handle_posix.c.ipp:356-361`).

1. **Before any `siginstall`.** The Windows unclaimed-raise path
   (`stdc_raise_initiated_exception_was_unclaimed` -> `false`,
   `thrd_signal_handle_windows.c.ipp:403-409, 466-473`) exists only when the library's
   vectored handler is installed — i.e. only after `siginstall`
   (`install_sighandler_impl`, `:576-595`). A `stdc_raise(SIGUSR1, NULL, NULL)` on a
   thread that has not installed the library raises the user-defined exception
   `0x40000001` with no vectored handler and no enclosing library `__except`: it reaches
   `UnhandledExceptionFilter` and Windows Error Reporting terminates the process, for both
   user-range codes (`0x40000000|signo`) and genuine codes (SIGSEGV -> `0xC0000005`).
2. **Installed signal, unclaimed by everyone.** When the map has an entry but
   `global_handler.front == NULL` (no global deciders) and no `sigguarded` frame covers
   the raise, the map-entry branch records the V5 dedup and returns
   `EXCEPTION_CONTINUE_SEARCH` with **no check of `stdc_raise_initiated_exception`**
   (that escape hatch exists only in the no-map-entry branch, `:456-473`): the library's
   unhandled filter runs the empty global pass and returns `CONTINUE_SEARCH`, the vectored
   continue handler returns the deduped `CONTINUE_SEARCH`, and the exception is unhandled
   -> Windows Error Reporting terminates the process; `stdc_raise` never returns. The
   identical call sequence on POSIX walks the (empty) frame chain, finds the map entry,
   and hands off to the previously installed handler, returning `true`.

Both contradict the header's documented contract "returning false if we have no decider
installed for that signal" (`thrd_signal_handle.h:533-563`) on Windows, and the two
backends' `stdc_raise` behaviour differs for the identical call sequence. (The proposal
permits "implementation-defined if this function ever returns", so this is a
header-doc/backend-parity defect rather than a wording violation.) No in-tree test
exercises scenario 2 — every Windows raise test either has a claiming decider, a guarding
frame, or an uninstalled signal (`thrd_signal_handle_test.c`, `thrd_sigfpe_test.c`,
`sigguarded_tss_init_test.c`, `post_uninstall_reentry_test.c`,
`stdc_raise_uninstalled_test.c`, `stdc_raise_null_info_test.c`). Fix direction: treat a
software raise (per-thread `stdc_raise_initiated_exception`) with a map entry but no
claiming decider like the no-entry case — set
`stdc_raise_initiated_exception_was_unclaimed` and return `EXCEPTION_CONTINUE_EXECUTION`
so `RaiseException` returns and `stdc_raise` reports false, or install a
`RaiseException`-specific continuation path. (Distinct from the case where the user's own
`__except` catches the raise and `stdc_raise` returns `true` — the zero-decider facet of
`IGND`.)

### `SUST` [wontfix] [code-level, Low] `siguninstall_system()` is a non-functional stub
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`thrd_signal_handle_common.ipp.ipp:555-563` — the function only validates `version == 0`
and returns 0; it installs/removes nothing. The header documents it as "Uninstall a
previously system installed signal guard", but no system installation exists anywhere in
the codebase. An API that reports success for an operation it never performs is a latent
trap for future callers (and for the eventual C standard library integration this library
targets).

### `SKIP` [wontfix, superseded in part] `siginstall` silently skips un-installable signals yet returns a valid handle
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

**Rev-5 amendment (2026-08-19):** the skip half of this finding is now
*conforming*: rev-5 7.14.2.5 p4 explicitly permits exactly this — "a signal
number that cannot be installed for reasons specific to the implementation is
silently not installed and the call reports success. It is implementation-defined
which signal numbers are not installed; an implementation should document the set
of signal numbers for which it silently does not install." The implementation
skips `SIGKILL`/`SIGSTOP` and the Fil-C-reserved signals and documents the
behaviour (header + this finding), so that half is resolved by the new wording.
The *reverse* arm now deviates: when `install_sighandler` fails for a signal
(e.g. `sigaction` returns an error for a signal the platform will not let the
library catch — the prototypical "cannot be installed for reasons specific to
the implementation" case), the implementation rolls back and returns NULL
(`thrd_signal_handle_common.ipp.ipp:552-586`) where p4 requires the signal to be
silently not installed and the call to report success. Only genuine resource
exhaustion (malloc/calloc failure) plausibly qualifies as "the installation was
unsuccessful" (p5) and justifies NULL; a per-signal `sigaction` failure does
not. Either treat per-signal install failures as silent skips (matching
`SIGKILL`/`SIGSTOP`), or obtain a wording change permitting the rollback.

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
zero-initialised `tss_async_signal_safe_t` handle (`thrd_signal_handle_common.ipp.ipp:328-333`),
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

### `SJSP` [wontfix] `_setjmp` vs `setjmp` inconsistency for header-only consumers; mask-restore semantics differ
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`WG14_SIGNALS_HAVE__SETJMP` is set only on the compiled library target
(`CMakeLists.txt:176-178`, PRIVATE). Header-only consumers never get the definition and
always use `setjmp` (saving/restoring the signal mask) even when `_setjmp` is available.
Not a correctness bug but a silent performance/behaviour split between the two modes —
with `setjmp` the mask saved at the `setjmp` is restored on `longjmp`, and combined with
`SA_NODEFER` handlers this can silently unblock/block signals relative to the interrupted
context (the former `SJMS`, merged 2026-08-18). Platform-dependent.

### `WXMS` [wontfix] [build, minor] MSVC builds lack the `-Werror` equivalent
**Adjudicated wontfix 2026-08-18:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`CMakeLists.txt:188-192`: GCC/Clang get `-Wall -Wextra -Wpedantic -Werror`; MSVC gets
`/W4 /experimental:c11atomics` with no `/WX`. All warnings that would break a strict
GCC/Clang build are invisible in the Windows CI leg.

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

`test/thrd_sigfpe_test.c:62-67` works around the lack of integer-divide trapping on some
architectures with `sigfence` and a `stdc_raise(SIGFPE)` fallback. x64 does trap integer
divide-by-zero (`EXCEPTION_INT_DIVIDE_BY_ZERO`, reverse-mapped to SIGFPE), so the real-fault
SEH path *is* exercised on Windows x64; the fallback fires only on non-trapping
architectures (ARM64). The test's coverage therefore varies per architecture.

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
since the abandon fix — `sigdecider_abandon`/`_resume` -> `LOCK(state->lock)`).
If a signal is delivered while the interrupted thread is itself holding the same lock,
the handler spins forever — a silent deadlock. The header's "usually async signal safe"
claim (`thrd_signal_handle.h:328-347`) does not cover re-entrancy.

This subsumes the former `LEAK` finding (merged 2026-08-18): the original
never-returning-decider refcount leak was fixed 2026-08-17 (`sigdecider_abandon()` drops
both per-raise references; verified by the global-abandon sections of
`test/guard_abandon_test.c`), and the follow-on in-lock reference adjustments
(`thrd_signal_handle_posix.c.ipp:485-559`, Windows `:433-529`) then made
`sigdecider_abandon`/`_resume` — documented `THREADSAFE ASYNC-SIGNAL-SAFE`
(`thrd_signal_handle.h:563,577`) — take `state->lock` inside a documented-valid
call pattern: a decider calling `sigdecider_abandon()` from a signal handler whose
interrupted thread holds that lock (e.g. mid-`siguninstall`/`signal_decider_destroy`)
spins forever. Fixing SPIN (lock-free atomic formulation, or signal masking around the
lock) is what the abandon/resume deadlock needs.

**Rev-5 extension (2026-08-19):** 7.14.2.8 p2 makes `signal_decider_destroy`
async-signal-safe "only when it is called by a signal decider to destroy the storage of
the decider that is currently executing" — i.e. in that context the wording requires
async-signal-safety. The implementation's destroy in that exact context still
`LOCK(state->lock)` (the spinlock, once per signal in the handle's set) and
`free(p)`/`free(node)` (`thrd_signal_handle_common.ipp.ipp:736-838`) — neither the
spinlock nor `free` is async-signal-safe, so the wording's claim does not hold on the
implementation for the one context in which the wording requires it.

### `ALOC` [wontfix] First signal delivery on a fresh thread performs allocation inside the handler
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`stdc_raise` -> `sig_global_tss_state_init` -> `calloc` + `thread_atexit` (which does
`std::vector` allocation / possibly throws) on the first call per thread
(`thrd_signal_handle_common.ipp.ipp:296-313`, `thread_atexit.cpp.ipp:71-85`). If the
first signal ever delivered to a thread arrives before any library call on that thread,
malloc and C++ heap operations run inside the handler (not async-signal-safe; risk of
deadlock on the heap lock). The docs recommend pre-calling `stdc_raise(0, ...)`; on Linux
this works, but the safety relies entirely on the user reading the docs.

### `TAFL` [wontfix] tss_async_signal_safe_thread_init does not roll back on `thread_atexit` failure
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`tss_async_signal_safe.c.ipp:228-249`: the map entry and `state->count` are committed
before `thread_atexit` is called; if it returns -1 the caller sees failure but the entry
and count remain, and no thread-exit cleanup will ever run for this thread. (On the
async-safe TLS path, `sig_global_tss_state_init` has the same pattern — it sets `*state =
mem` *before* `thread_atexit(free, mem)`; on registration failure the pointer stays set
and the next call silently succeeds with a leaked `mem`.) The failure never even surfaces
on the C path: `thread_atexit.c.ipp:70-75` calls `__cxa_thread_atexit()` and ignores its
return, unconditionally returning 0 (the former `CXAT`, merged 2026-08-18), so TAFL's
rollback is unreachable there and the leak is invisible; on the C++ path with exceptions
disabled the would-be -1 is converted into `std::terminate` (`TAOM`).

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
**Wording conformance, re-verified 2026-08-19** against the rev-5 wording: it claims
"`sigfence()` is *async-signal-safe*" (7.14.1) unconditionally, so on the
MSVC/`DISABLE_INLINE_ASM` fallback path the shared `sigfence_sink` barrier-slot writes
make the macro not strictly async-signal-safe and that wording claim does not hold on
those configurations. **Also re-verified 2026-08-19:** the rev-5 5.2.2.4 p7 guarantee
("no access to the memory is performed on the other side of the call from where it is
sequenced") rests on the compiler treating the volatile sink accesses as scheduling
barriers for plain stores — formally, plain stores to bytes of a fenced object other
than the one volatile-read can be sunk past the barrier when the abstract machine cannot
observe the difference. All mainstream compilers in practice treat volatile as a
scheduling barrier, so this is a QoI assumption, but the formal guarantee is only
delivered by the GNU/Clang asm path (`"+m"` operands), not by the fallback.

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

### `SFQL` [wontfix] sigfence on GNU compilers requires lvalues; non-lvalues fail to compile
**Adjudicated wontfix 2026-08-17:** no remediation scheduled; see the §4 wontfix legend. The analysis below is retained for the record.

`WG14_SIGNALS_SIGFENCE_IMPL_1(a)` expands to `__asm__ volatile(";" : "+m"(a) : : "memory")`.
The `+m` operand must be an lvalue: `sigfence(42)` or `sigfence(x + 1)` is a hard compile
error. Verified (`error: invalid lvalue in asm output`); the 0-arg form compiles. The
header now documents the requirement ("Any variable in the argument list MUST be a
lvalue", `thrd_signal_handle.h:248-249`), so the failure is documented if still a bare
compiler error.

### `TSSD` [wontfix, code-level, security, Low] `tss_async_signal_safe_destroy` / `signal_decider_destroy` double-destroy is an unguarded use-after-free

Two instances of the same double-destroy defect, merged 2026-08-18 (from the former
`DEDE`):

1. `tss_async_signal_safe.c.ipp:114-138`: a **second** `destroy` on the same (non-NULL)
   handle calls `LOCK(mem->lock)` on freed memory and then `free(mem)` again — a double-free /
   use-after-free, i.e. a potential security vulnerability. Post-destroy `get`/`thread_init`
   on the freed handle crash identically. (AGENTS.md rule 9: NULL/zeroed *input* handles
   crashing is intended fail-fast and is not a defect; the freed-handle double-destroy is
   out of scope of that rule because it is a use-after-free, not a NULL input.)
2. `thrd_signal_handle_common.ipp.ipp:757` (`free(p)`) unconditionally frees the decider
   handle; a second `signal_decider_destroy` on the same pointer reads freed memory before
   the double-free. No guard exists.

**Wontfix rationale (adjudicated 2026-08-16).** Destroying an already-destroyed
identifier is undefined behaviour under the governing contracts: C11 7.26.6.3
`tss_delete` and POSIX `pthread_key_delete` define only a single deletion, the N3924
wording's `tss_async_signal_safe_destroy` (7.30.6.6) and `signal_decider_destroy` (7.14.2.8)
do the same, and the header's THREADSAFE NOT REENTRANT note does not address double-destroy.
Standard-library implementations tolerate a second delete only because their keys
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
  global-decider `invoke_recovery` divergence, the `pcpp` future work, and (since
  2026-08-18) the `MLAS` setjmp UB — but still none of the open fixable findings above;
  the plan files remain the only inventory.
- **AC1 [docs-hygiene, Low]** source comments and the CI YAML cite finding IDs from the
  pre-renumbering scheme (`analysis.md 5.10`, `2.9`/`W11`, `1.8`/`C3`/`Y10`, cited by
  `test/thrd_sigfpe_test.c`, `test/sigfence_fence_test.c`, `test/header_only_build_test.cmake`,
  `test/header_only_c_consumer/main.c`, `ci.yml`) that no longer resolve in this document;
  retarget them to the surviving codes or to `ci.yml` itself.
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
  (the per-candidate `_wg14_signals_cxa_result` is now `unset(... CACHE)` before each
  probe, `:56`), and `WG14_SIGNALS_FT_PROBE_0..4` (`:142-154`). The remaining cached values
  persist across reconfigures; switching the compiler (or the libc) in the same build
  directory reuses the stale probe outcomes with no diagnostic — the feature-test-macro
  ladder (`_ftm_choice`) and the `_setjmp` selection can silently come from a previous
  toolchain. Fix direction: `unset(... CACHE)` the probe variables at the top of each
  configure, or key them on `CMAKE_C_COMPILER`.
- **POSIX `stdc_raise` dead store.** `thrd_signal_handle_posix.c.ipp:332` — the
  `sig_decision_resume_execution` case executes `frame = frame->prev;` immediately
  before `return true;`; the store is dead. Harmless, but it reads as if the walk
  continued; drop the assignment.
- **`siginstall_rollback_test` is excluded from Fil-C builds:** `--wrap` is unusable under
  Fil-C's symbol pizlonation (the linker's rewrites never match the mangled names), so the
  test is excluded (`test/CMakeLists.txt`, `CMAKE_C_COMPILER MATCHES "[Ff][Ii][Ll]-?[Cc]"`);
  rollback coverage remains on the GCC/Clang Linux and FreeBSD legs. (The earlier
  `--wrap` + hidden-visibility link breakage that motivated the exclusion was fixed by
  giving the interposer default visibility.)

### Minor proposal-conformance notes (N3924 rev 5 wording)

- **`signal_decider_destroy` reentrancy.** The N3924 7.14.2.8 wording and the header now
  document that calling `signal_decider_destroy` from within a decider function is
  permitted (the free of a currently-executing decider's node is deferred until the raise
  completes); verified by `test/decider_reentrant_destroy_test.c`. `signal_decider_create`
  remains not reentrant (allocation and warning-path `fprintf` in handler context).
- **`sigaddset`/`sigdelset`/`sigismember` out-of-range signo on macOS/BSD.** The wording
  (N3924 7.14.2.2) now requires a negative value for an out-of-range signo (the C standard
  never states errno-setting in Returns clauses; the POSIX imperative -1 with
  `errno = EINVAL` remains a POSIX-level mandate that the Windows in-tree helpers and the
  glibc/musl host libcs implement). On POSIX the reference
  implementation delegates to the host libc: glibc/musl conform, but the macOS/BSD libcs
  deviate (probe-verified: `sigaddset(&ss, 0)` returns 0 without errno; `sigismember` is
  a shift-count macro, undefined behaviour out of range) — documented at
  `thrd_signal_handle.h:41-53`; `sigset_helpers_contract_test` asserts the out-of-range
  path on Windows only.
- **Async-signal-safe claims.** The rev-5 wording marks both `sigguarded` (7.14.3.1) and
  `stdc_raise` (7.14.2.9) "async-signal-safe" with no per-thread-priming qualification
  (the rev-4 priming restriction was dropped from the wording; the analysis's earlier
  "internally inconsistent" note is obsolete): `stdc_raise` is undefined when called
  during the handling of a signal by a thread on which neither it nor `sigguarded` has
  previously been called (per-thread priming), while `sigguarded`'s only qualification is
  the activation precondition (no call of `siginstall` with a signal set containing that
  signal number performed) — yet both share the same per-thread init path
  (`sig_global_tss_state_init` → `tss_async_signal_safe_thread_init` → allocation).
  Under the current wording, `sigguarded` called from a handler on a fresh unprimed
  thread for an installed signal is *not* undefined behaviour, but the reference
  implementation allocates inside the handler (findings `ALOC`/`SPIN`), so it does not
  satisfy the wording's claim in that case; the header's "USUALLY ASYNC-SIGNAL-SAFE"
  (`thrd_signal_handle.h:499-518, 533-563`) is the honest contract. `stdc_raise`'s
  claim is likewise satisfied only after the per-thread setup call
  (`stdc_raise(0, nullptr, nullptr)`) has been made on the thread. Decider invocation is
  additionally gated on activation in the wording ("Signal deciders are invoked for
  `signo` only if `signo` is activated"), which both backends violate — see finding
  `ACTV`. The `signal_decider_destroy`-in-decider async-signal-safety claim (7.14.2.8
  p2) is likewise unsatisfied — see the `SPIN` extension.
- **Decider requirements scope (7.14.4).** The rev-5 wording factored the shared
  handler/decider requirements into 7.14.4 and scoped the async-signal-safe call
  restriction to signals occurring *other than* as the result of calling `abort` or
  `raise`, so a decider invoked by `stdc_raise` in normal context may call
  non-async-signal-safe functions. Both backends instead demand unconditional decider
  async-signal-safety ("You must NOT do anything async signal unsafe in here!",
  `thrd_signal_handle_posix.c.ipp:341`, `thrd_signal_handle_windows.c.ipp:341`), and the
  header's decider contract (`thrd_signal_handle.h:499-518`) is unqualified. Stricter
  than the standard is permitted (QoI), but a program conforming to the wording's
  relaxed scope would be out of the reference implementation's contract; recorded as a
  deviation.
- **`sigguarded` with `recovery == NULL`.** Allowed by both backends (POSIX falls through
  to outer frames/globals, `thrd_signal_handle_posix.c.ipp:335-343`; Windows
  `EXCEPTION_CONTINUE_SEARCH`, `thrd_signal_handle_windows.c.ipp:273-279`) and exercised
  by `recovery_null_loop_test`; the N3924 rev 5 wording (7.14.3.1) now defines it: a
  decider returning `sig_decision_call_recovery` is treated as if it had returned
  `sig_decision_next_decider`.
- **`tss_async_signal_safe_thread_init` return values.** The wording promises
  `thrd_success`/`thrd_error`; the implementation propagates the user `attr->create`
  callback's arbitrary nonzero return verbatim (`tss_async_signal_safe.c.ipp:222-226`), so
  a create returning e.g. 1 yields thread_init returning 1 (neither 0 nor -1).
- **`sighandler_info` / `stdc_siginfo` field-name drift.** The struct members match the
  wording (`signo`, `error_code`, `addr`, `value`, `raw_info`, `raw_context`) — checked,
  no deviation; recorded here only to note it was verified.
- **Rev-5 `stdc_raise` p5 NULL-`raw_info` contract.** 7.14.2.9 p5: "If `raw_info` is a
  null pointer, the `raw_info` member is a null pointer, the `error_code` member is
  zero, and the `addr` member is a null pointer." POSIX conforms
  (`thrd_signal_handle_posix.c.ipp:228-252`); Windows cannot — `raw_info` is always the
  (possibly synthesized) `EXCEPTION_RECORD` and `error_code`/`addr` derive from the
  record's parameters (the `MUTR` wording-conformance note). Verified unchanged against
  rev 5.
- **Rev-5 p11 siginfo lifetime.** 7.14.1 p11: the `struct stdc_siginfo` lifetime extends
  until handling completes, or until the recovery function returns when recovery is
  invoked. POSIX recovery reads the frame-copied `rsi` on the `sigguarded` stack
  (`thrd_signal_handle_posix.c.ipp:392-394, 409-411`); Windows recovery reads
  `sigguarded`'s own local (`thrd_signal_handle_windows.c.ipp:299-338`) — both satisfy
  the lifetime rule. Verified, no deviation.
- **Rev-5 p11 value propagation.** "the `value` member is set to the `value` argument of
  the call of `sigguarded`... or `signal_decider_create`... that installed the decider";
  a decider that mutates `rsi->value` (as the 7.14.1 EXAMPLE 1 does) must have the
  mutation visible to recovery. POSIX copies the raise's `rsi` into the frame after the
  decider returns (`:392-394, 409-411`), Windows passes the same `rsi` local to recovery
  — both propagate the mutation. Verified, no deviation.
- **Rev-5 p13/p15 interplay with `sigdecider_abandon` in a *global* decider.** The
  Windows abandon pops the top `sigguarded` frame when a global decider abandons
  (`thrd_signal_handle_windows.c.ipp:470-475`); POSIX does not. Both are inside
  undefined-behaviour territory for the never-returning decider (7.14.3.2 p5 makes
  returning after abandonment without resume UB), so no wording deviation is claimed;
  the backend divergence is recorded for completeness.
- **Rev-5 p6 `sigguarded` NULL arguments.** 7.14.3.1 p4 makes NULL `signals`/`guarded`/
  `decider` undefined behaviour; both backends abort (fail-fast, per AGENTS.md rule 9) —
  defining UB is conforming. Verified.
- **Rev-5 p6 recovery-NULL.** 7.14.3.1 p6 ("a decider returning
  `sig_decision_call_recovery` is treated as if it had returned
  `sig_decision_next_decider`") — POSIX falls through to the outer frame/global pass
  (`thrd_signal_handle_posix.c.ipp:400-408`), Windows `EXCEPTION_CONTINUE_SEARCH`
  (`thrd_signal_handle_windows.c.ipp:292-293`); both conform, and `recovery_null_loop_test`
  covers the POSIX side. Verified.
- **Rev-5 p11 global-decider ordering.** 7.14.1 p11: `callfirst` true, most recently
  installed first; `callfirst` false, installation order. `LIST_INSERT_FRONT`/`BACK`
  under `state->lock` (`thrd_signal_handle_common.ipp.ipp:719-728`) implement exactly
  this. Verified.
- **Rev-5 p15 activation counts.** Incremented per `siginstall` member, decremented per
  `siguninstall` member, container retired at zero (`thrd_signal_handle_common.ipp.ipp:
  456, 501-513`); the reference-counted container also survives in-flight raises.
  Verified.
- **Rev-5 7.14.2.6 handle invalidation.** "The handle becomes invalid after a successful
  call to this function" — `siguninstall` frees the handle block (`:614`); a second
  `siguninstall` on the same handle is therefore a use-after-free, which the wording
  makes UB ("The behavior is undefined if `handle` is not the value returned by a prior
  call... that has not yet been uninstalled"). Conforming.
- **Rev-5 7.30.6.7 repeated `thread_init`.** "subsequent calls do not invoke the `create`
  function pointer again, do not change the thread-specific storage pointer, and return
  `thrd_success`" — the map-get hit path returns 0 without invoking `create`
  (`tss_async_signal_safe.c.ipp:290-295, 342-344`). Verified, no deviation (modulo the
  `UCRE` unlocked-window race).
- **Rev-5 7.30.6.8 `get` precondition.** "The `tss_async_signal_safe_thread_init`
  function... shall have been called on the same thread for the instance identified by
  `val`, in which case the thread-specific storage pointer created at that time... is
  returned; otherwise the behavior is undefined." The implementation returns NULL for an
  uninitialised thread — a defined, benignly stronger behaviour (finding `TSSG`).
  Conforming.

---

## 4. Priority-ordered remediation summary

The severity label in the tables classifies each finding's worst-case impact; the row
order is the remediation priority, which applies the criteria below to the label *and*
the finding's trigger likelihood. It is not a strict severity sort: the 34 adjudicated
wontfix findings rank *last* regardless of severity, and the remaining Med item
`TLSD` follows Low items that are more dangerous in practice.
The main table lists the fixable findings; the wontfix findings have their own
summary table below it. The ranking criteria below describe the original priority
assessment; items adjudicated wontfix (see tier 13) are retained in the
wontfix table regardless of where their tier placed them.

Where merged findings touch the same machinery (`DEIN` extends the `UNTL` trigger,
`IGND` covers both the SIG_IGN hand-off and the zero-decider `stdc_raise` return
contract), the earlier finding is referenced
rather than duplicated. Every finding is cited by its stable four-letter code.
On 2026-08-18 the following duplicates were merged into their primary finding (which
absorbs them): `ZERO` into `IGND`, `REEN` into `UCRE`, `CXAT` into `TAFL`, `LEAK` into
`SPIN`, `WRET` into `PREI`, `SJMS` into `SJSP`, `MSQR` and the raw_context note into
`MUTR`, and `DEDE` into `TSSD`; `WFRM` was removed as fixed (the Windows dispatch now
runs the thread-local frame deciders before the global-decider pass).

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
   missing rollback on `thread_atexit` failure (`TAFL`) and the unlocked-create
   re-entrancy/race window in `thread_init` (`UCRE`, tier 12).
4. **Data races and setjmp-family UB.** C/C++ memory-model violations that current
   compilers tolerate today. The thread-exit deinit reading `state->val` unlocked against a
   concurrent destroy (`DEIN`) leads the tier; then the benign double-checked `sigfillset`
   write race (`SIGF`), modified-local-after-setjmp (`MLAS`), skipped C++ destructors
   (`CPPD`), and the `sigfence` MSVC fallback's shared-sink race (`SFNK`). Latent UB, not
   today's crashes.
5. **Silent alteration of host-process semantics.** POSIX install flags and
   default-action handling that change the behaviour of the host process for the whole
   tenure of an install: `SA_NOCLDWAIT`/no `SA_RESTART`/`SA_NODEFER` on the default
   `siginstall(NULL)` path used by every test and the README (`FLGS`). (The stale-TID
   reuse finding — `TIDR` — was fixed: the `tss_async_signal_safe` map keys now carry a
   per-thread generation.) Default-path triggers precede rarer ones. (The stop/continue and
   realtime re-raise discarding the library's handler — `DFLT` — was fixed:
   `invoke_sigaction` now restores the installed handler after the default action. The
   unknown-signal fallback — `UNKN` — was fixed the same way: `raw_signal_handler` no
   longer resets the kernel handler to `SIG_DFL` before the pass-on.)
6. **Error-path and lock hygiene.** Allocation-failure correctness and lock discipline in
    non-hot paths (`TAOM` OOM terminate
    with exceptions disabled, `FPUN` function-pointer type pun).
7. **API-contract and error-reporting violations.** Documented behaviour is not
   delivered, including the N3924 wording's return contracts. Leads with the findings
   that can crash or silently lie about what happened: `stdc_raise`
   reporting success for a silently-ignored signal or with zero deciders called (`IGND`),
   Windows `stdc_raise` terminating the process via WER for an unclaimed raise while POSIX
   returns/hands off in-process (`PREI`), the no-op `siguninstall_system` stub (`SUST`),
   `siginstall` silently
   skipping SIGKILL/SIGSTOP/Fil-C signals yet reporting success (`SKIP`),
   `signal_decider_create` before `siginstall` silently losing the decider (`PRCR`). The three public
    identifier/signature deviations from the wording are all fixed: `VSDT` (the Windows
    `sigemptyset`/`sigfillset`/`sigaddset`/`sigdelset`/`sigismember` helpers now return
    `int` as the N3924 7.14.2.1/7.14.2.2 synopses require, and both backends implement
    the POSIX contract -- 0 on success, -1 with errno = EINVAL for an out-of-range signo
    in sigaddset/sigdelset/sigismember), `ENUM` (the
    `sig_decision` member is now `sig_decision_call_recovery`), and `TYDF` (the error-code
    typedef is now `stdc_siginfo_error_code_t`), so the tier no longer ends in loud
    compile-time API-identifier failures.
8. **Portability and platform gaps.** Compile failures or safety-claim gaps outside the
   exercised CI matrix. The Med items lead this tier because they also break the
   async-safety claim or are real functional gaps on a supported platform: optimistic
   async-safe-TLS auto-detection (`TLSD`), `mach_thread_self`/`pthread_getthreadid_np` not
   async-safe on Apple (`MAST`), and MSVC CRT signals bypassing SEH (`CRTS`). The rest are
   exotic-platform build/portability risks (MinGW, `_setjmp`/`setjmp` split, `fork()` TID
   caches, undefined `NSIG`).
9. **Build-system issues.** Configuration/package defects, ranked before test issues
   because they gate what CI can observe: MSVC missing `/WX` (`WXMS`), the unconditional
   CXX requirement (`CXXR`), the undeclared C++ runtime dependency (`CPPR`), and CI gaps
   (`CIGA`).
10. **Test-harness defects.** Flaws in the test shims that can mask real failures or
      write out of bounds: test storage exhaustion (`STOR`), and the trap-dependent SIGFPE
      test (`TRAP`).
11. **Minor, cosmetic, and Windows edge-case quirks.** Behavioural divergences with low
    practical impact: the cryptic `sigfence` 9-argument error (`SFAR`) first as it affects
    every user's compile experience, then the Windows-specific quirks (`SABA` SIGABRT
    non-continuable, `MUTR` mutated `EXCEPTION_RECORD`, `UFLT` clobbered unhandled filter,
    `NDBS`/`GDIR`/`CATM`/`SFPD` set and `error_code` divergences).
12. **Documented limitations and open design notes.** Behaviour that is either explicitly
    documented or needs a design decision before a fix is worthwhile: the unlocked
    `attr.create` callback (`UCRE`, covering the cross-thread race and the re-entrancy
    double-create), the now-documented `sigfence` lvalue requirement (`SFQL`), and the
    documented precondition deviations of `tss_async_signal_safe_destroy` and
    `tss_async_signal_safe_get` (`TSSG`).
13. **Adjudicated wontfix.** Thirty-four findings were adjudicated not to fix:
    the double-destroy pair `TSSD` (covering `signal_decider_destroy` too; 2026-08-16 —
    double-
     destroy is undefined behaviour under the C11/POSIX/N3924 contract and, as this
     reference implementation keeps raw-pointer handles that `destroy()` frees, no guard
     will be added; see the heading body), on 2026-08-17: `UNTL`, `SPIN`, `ALOC`,
     `TAFL`, `DEIN`, `SIGF`, `CPPD`, `SFNK`, `FLGS`, `TAOM`, `FPUN`,
     `MAST`, `CRTS`, `MING`, `FORK`, `NSIG`, `NDBS`, `UECL`, `SFQL`, and
     on 2026-08-18: `MLAS`, `PREI`, `SUST`, `SKIP`, `FWTF`, `SJSP`,
     `WXMS`, `CXXR`, `CIGA`, `TRAP`, `SFAR`, and on 2026-08-20: `RAIS`,
     `HNDF`, `ABRS`.
     Because no remediation is expected for them, they rank **last**, below every
     fixable finding, despite some being among the higher-severity or weaponisable
     items in the inventory.

Tie-breakers within a tier: severity label; then whether the trigger is a realistic user
pattern versus an exotic or timing one (confirmed reproductions and deterministic
corruption beat narrow-window UB); then confirmed reproduction over code-level analysis;
then backend scope.

| Code | Category | Severity | Issue |
|---|---|---|---|
| `NRAI` | windows | Low | `stdc_raise` of an invalid signo raises a real SEH exception (7.14.2.9 p6 "returns false without raising a signal"; reaches WER without `siginstall`) |
| `WVLD` | windows | Low | Windows sigset helpers accept signo 23..32 (not valid signal numbers; `sigfillset` sets bits for undefined signals) |
| `IGND` | contract | Low | `stdc_raise` true when raise silently ignored (POSIX SIG_IGN/default-ignore hand-off, POSIX zero-decider, and Windows deciders-called-but-unclaimed; Windows zero-decider arm fixed 2026-08-20) |
| `ACTV` | contract | Low | deciders consulted without activation on BOTH backends (POSIX frame walk; Windows frame filter) — rev-5 7.14.1 p15 |
| `PRCR` | contract | Low | `signal_decider_create` before `siginstall` silently loses the decider (rev-5 7.14.2.7 p5) |
| `TLSD` | portability | Med | async-safe TLS detection too optimistic; forced-on-Apple unsafe |
| `PTHD` | portability | Med | pthread-key `thread_atexit` fallback drops every callback on Darwin (TLS torn down before key destructors) **[confirmed]** |
| `CPPR` | build | Low | C++ runtime dependency undeclared |
| `STOR` | test | Low | test storage exhaustion |
| `SABA` | windows | Low | `stdc_raise(SIGABRT)` non-continuable; resume loops only without a raise frame |
| `MUTR` | windows | Low | `stdc_raise` mutates caller's `EXCEPTION_RECORD`; params masquerade; raw_context dropped on full record |
| `UFLT` | windows | Low | `siguninstall` clobbers app filter (install-time overwrite too) |
| `GDIR` | contract | Low | global decider `invoke_recovery` divergence |
| `CATM` | cpp | Low | `calloc` C++ `std::atomic_uint` lifetime |
| `SFPD` | windows | Low | `stdc_raise(SIGFPE)` code divergence |
| `DRGN` | windows | Low | drain-time re-registration dropped on MSVC TLS-directory path; UB on C++ vector path |
| `UCRE` | contract | Low | `thread_init` unlocked `attr.create` (cross-thread race; re-entrancy double-create) |
| `TSSG` | docs | Low | get precondition/ASYNC-SIGNAL-SAFE claim undocumented (destroy half resolved by rev-5 7.30.6.6 p3) |

### Wontfix findings (adjudicated not to fix)

| Code | Category | Severity | Issue |
|---|---|---|---|
| `RAIS` (wontfix) | contract | Med | `stdc_raise` of a non-activated signal does not behave as-if `raise()` (7.14.2.9 p3/p4): `signal()` handler never called, default action never taken |
| `HNDF` (wontfix) | contract | Med | activated-signal hand-off to the previously installed handler where 7.14.1 p14 requires default handling and forbids using the `signal`-function handler |
| `ABRS` (wontfix) | contract | Low | `sigdecider_abandon_resume` aborts when a nested signal's processing changed `tss->front` between abandon and resume, in a wording-valid call sequence |
| `MLAS` (wontfix) | ub | Low | modified-local-after-setjmp UB |
| `PREI` (wontfix) | windows | Med | unclaimed `stdc_raise` terminates via WER only before any `siginstall` (facet 2 — installed signal, no decider/frame/`__except` — fixed 2026-08-20); POSIX returns/hands off |
| `SUST` (wontfix) | contract | Low | `siguninstall_system` no-op stub |
| `SKIP` (wontfix, superseded in part) | contract | Low | silent skip of SIGKILL/SIGSTOP/Fil-C signals now conforming (rev-5 7.14.2.5 p4); the reverse arm — NULL + rollback on per-signal `sigaction` failure where p4 requires silent skip + success — remains open |
| `FWTF` (wontfix) | windows | Med | fallback-TLS `sig_global_tss_state()` NULL-deref in vectored function on fresh threads |
| `SJSP` (wontfix) | portability | Low | `_setjmp` vs `setjmp` inconsistency for header-only consumers (mask-restore semantics differ) |
| `WXMS` (wontfix) | build | Low | MSVC lacks `/WX` |
| `CXXR` (wontfix) | build | Low | project requires CXX |
| `CIGA` (wontfix) | build | Low | CI gaps (Windows C_STANDARD, benchmarks, FreeBSD) |
| `TRAP` (wontfix) | test | Low | SIGFPE test trap-dependent (real-fault SEH path exercised on x64; fallback only on non-trapping archs) |
| `SFAR` (wontfix) | header | Low | `sigfence` >8 args cryptic error |
| `UNTL` (wontfix) | memory | Med | `siguninstall` during another thread's `sigguarded` frees live TSS (fallback) |
| `SPIN` (wontfix) | async | Med | Spinlock not async-signal-safe (incl. `sigdecider_abandon`/`_resume` in handler context) |
| `ALOC` (wontfix) | async | Med | first delivery allocates in handler |
| `TAFL` (wontfix) | leak | Low | `thread_init` no rollback on `thread_atexit` failure (C path swallows the failure) |
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
| `SFQL` (wontfix) | header | Low | `sigfence` requires lvalues (documented) |
| `TSSD` (wontfix) | security | Low | `tss_async_signal_safe_destroy` / `signal_decider_destroy` double-destroy = unguarded UAF |

*(wontfix) = adjudicated not to fix, per the C11/POSIX/N3924 contract, with the
rationale recorded in the heading body. 2026-08-16: `TSSD` (double-destroy is
undefined behaviour; no guard will be added to a raw-pointer handle that `destroy()`
frees), and the *uncooperative* arm of the removed `JLGS` (a decider that never returns
without calling `sigdecider_abandon`). 2026-08-17: `UNTL`, `SPIN`, `ALOC`, `TAFL`,
`DEIN`, `SIGF`, `CPPD`, `SFNK`, `FLGS`, `TAOM`, `FPUN`, `MAST`, `CRTS`, `MING`,
`FORK`, `NSIG`, `NDBS`, `UECL`, `SFQL`. 2026-08-18: `MLAS`,
`PREI`, `SUST`, `SKIP`, `FWTF`, `SJSP`, `WXMS`, `CXXR`, `CIGA`, `TRAP`, `SFAR`.
2026-08-20: `RAIS`, `HNDF`, `ABRS`.
2026-08-18 also merged duplicate findings into their primaries: `ZERO`->`IGND`,
`REEN`->`UCRE`, `CXAT`->`TAFL`, `LEAK`->`SPIN`, `WRET`->`PREI`, `SJMS`->`SJSP`,
`MSQR`->`MUTR`, `DEDE`->`TSSD`.*
