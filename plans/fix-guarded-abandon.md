# Fix `JLGS`: keep the POSIX frame chain safe when a thread-local *decider* never returns (`guarded`/`recovery` must return)

## Goal

`plans/analysis.md` finding `JLGS` (row 1, confirmed, Med): `sigguarded` stores
the guard frame `current` as a local of `sigguarded` and links it on
`tss->front`, popping it only on the two normal exits. If control leaves the
guarded region without either pop — a never-returning *thread-local decider*
(`siglongjmp` to a caller-owned buffer, `_exit`, a loop) abandoning the whole
`guarded`/`sigguarded`/`stdc_raise` call chain — `tss->front` dangles. A later
`stdc_raise` (or a real signal delivered to the thread) walks
`frame->guarded`/`decider`/`buf` in re-used stack (verified ASan
`stack-use-after-scope`) and can `longjmp` into a dead environment.

**Return contract (user-confirmed, corrected 2026-08-16):** the `guarded` and
`recovery` functions MUST return — if either never returns, the behaviour is
undefined (the user's own `setjmp`/`longjmp`-class misuse, cf. C11 7.13.2.1).
The `decider` IS permitted to never return (N3924 7.14.1: "It is permitted for a
signal decider to never return"), so the library MUST remain memory-safe and
correct when a thread-local decider abandons the frame chain. This plan fixes
that case and documents the return contract.

Scope: POSIX backend frame chain (`thrd_signal_handle_posix.c.ipp`). Windows
`sigguarded` uses `__try/__except` and never pushes frames; its `stdc_raise`
raise-frame pinning is the separate `NDEC` finding (kept out of scope).
Never-returning *global* deciders are `LEAK` (kept out of scope).

## Research conclusions (decide the design)

1. **No portable liveness test exists.** Abandonment leaves no observable hook
   (cannot interpose `longjmp`), and any stack-address heuristic against the
   raise's own frame is unsound: live frames always satisfy
   `frame->home > raise_sp`, but post-abandonment code can re-descend to a
   deeper stack, so a stale frame can satisfy it too. A "skip frames you cannot
   trust" strategy can therefore silently drop *live* guards and is rejected.
2. **Sound-skip direction exists:** a live guard frame is an *ancestor* of every
   raise executed inside `guarded()`, so `home_mark` (a stack address captured
   in `sigguarded` at push time) is *strictly greater* than the raise's own
   stack address for every live frame. A frame with `home_mark < raise_sp` is
   provably not an ancestor → provably dead → safe to skip and keep walking
   outward (an abandoned inner guard with a still-live outer guard is handled:
   the dead inner frame is skipped, the live outer frame is consulted).
3. **Stale-frame memory access must be safe regardless.** Frames must not live
   on the abandonable stack at all — move them to per-thread heap frames so
   reads of abandoned frames are always defined memory.
4. **Residual, documented:** a stale frame whose `home_mark` is still greater
   than the raise SP (thread re-descended below the abandoned frame's position)
   may still be consulted; executing recovery into it is undefined behaviour.
   This is the user's own never-returning decider having abandoned the guard, and
   matches the documented contract that requesting recovery through an abandoned
   guard is UB (also C11 7.13.2.1's rule that a `jmp_buf` is invalid after its
   containing function terminates). The library guarantees no UAF and no
   dereference of reused stack; the residual equals the user's own misuse, not a
   library defect.
5. **`guarded`/`recovery` must return is a hard contract, already satisfied
   structurally:** the frame is unlinked *before* `recovery` is called (already
   true today, `thrd_signal_handle_posix.c.ipp:274-278`), so the recovery branch
   leaves the chain clean even under the contract; the normal `guarded` return
   path pops before `sigguarded` returns. Document the contract; do not
   restructure these paths.
6. **Co-fix `SJMP` (row 3):** with heap frames, publish the frame on
   `tss->front` only *after* `setjmp` returns, closing the publish-before-
   setjmp window in the same code region.
7. **`MLAS` (row 15) structurally mitigated:** with the frame no longer an
   automatic object, the C11 7.13.2.1 "modified non-volatile automatic object"
   UB rule no longer applies to `frame->rsi`; keep the field accesses as-is.

## Alternative design (researched 2026-08-16): two new public `sigguarded_abandon` / `sigguarded_abandon_resume` APIs

The alternative to the silent sound-skip heuristic: make abandonment *explicit*
and *retractable* via two new public APIs, callable from code running within a
`sigguarded` dynamic extent (the `guarded` body, a thread-local frame decider)
or from within a global decider. `sigguarded_abandon()` declares non-return and
fills a caller-provided implementation-defined `sigguarded_abandoned_state_t`;
`sigguarded_abandon_resume(state)` retracts it.
The concept is not in N3924 rev 4, so it is a wording extension; draft
normative sections (`7.14.4.2` and `7.14.4.3`) were added to
`docs/proposed-wording.md` on 2026-08-16 at the author's request and await
review.

### Idea and user-visible semantics

- `sigguarded_abandon(sigguarded_abandoned_state_t *state)` — the calling
  code declares it will never return through the guard machinery (its `guarded`
  body will never return, or its decider will never return to `stdc_raise`).
  The library immediately unlinks the thread-local guard frame from
  `tss->front`, so no subsequent raise on the thread consults a guard that is
  about to dangle, and fills `*state` (an implementation-defined structure,
  per the draft wording) with the retraction information. Returns 0 on success;
  nonzero (`*state` untouched) when called outside any guard/decider context.
  A null `state` is permissible: no retraction information is stored and the
  declaration is irrevocable.
- `sigguarded_abandon_resume(const sigguarded_abandoned_state_t *state)` —
  reverses a prior `sigguarded_abandon()` on the same thread: re-links the
  parked frame so the guard is consulted by raises again and the normal return
  path pops it. The retraction information is single-use and thread-bound;
  using it after the abandoned context has actually terminated is UB (the same
  class as C11 7.13.2.1's `jmp_buf` rule).

The motivating pattern is exactly the user's: code inside a guard believes it
will never return (it is about to `longjmp` to a caller-owned buffer, block in
an operation that might terminate the thread, enter a loop, etc.), declares
that belief to the library so the chain cannot dangle, and later — if it turns
out control stays in the guard and it *must* return — retracts the declaration
with `resume` and returns normally. Abandoning also gives a *defined* path for
`guarded`/`recovery` to never return (call `abandon` first), softening the
corrected return contract for code that opts in.

### What it buys over the sound-skip design

1. **Deterministic, residual-free abandonment (for cooperative code).** The
   library KNOWS a parked frame is dead; it can never be consulted and recovery
   can never be requested through it. Research conclusion 4's residual (a stale
   frame whose `home_mark` is still above the raise SP may be consulted;
   recovery through it is UB) disappears entirely for code that calls `abandon`.
2. **Retractable.** The silent design cannot "undeclare" a frame it inferred
   dead by stack address; `resume` is a capability only the explicit API has.
3. **A public hook for the `LEAK`/`NDEC` deferred-drain machinery** (see below),
   turning a research idea into a user-facing contract.

### What it costs / what it needs

1. **New public API surface**: two functions and an implementation-defined
   structure type (`sigguarded_abandoned_state_t`), docs, ABI, tests, and
   new WG14 wording sections.
2. **It does NOT fix the uncooperative case.** A decider that never returns
   *without* calling `abandon` is still the JLGS bug, and N3924 7.14.1 permits
   that without any cooperation. So either the sound-skip machinery stays as the
   safety net (recommended — the API is an accelerator/qualifier on top), or the
   contract is strengthened to "a never-returning decider must abandon first,
   else UB", which contradicts N3924's unconditional wording and is not
   recommended.
3. **The heap-frame restructure of the base design is a prerequisite.** Today's
   `sigguarded` pops via the `old` local saved at push time
   (`thrd_signal_handle_posix.c.ipp:271`, `:284`); a parked-then-resumed frame
   must instead pop via its own live `prev` (the chain may have changed while it
   was parked, and `old` would clobber it). The pooled per-thread heap frames +
   `tss->front = f->prev` restructure already in this plan are exactly what the
   API needs; with heap frames the parked frame's memory also stays valid even
   if the stack is unwound, keeping `resume` defined for longer.
4. **Per-frame and per-thread additions**: a `linked` flag on the frame, and a
   per-thread in-flight raise record in `sig_global_state_tss_state_t`
   (`thrd_signal_handle_common.ipp.ipp:267-282`) set by `stdc_raise` before each
   decider invocation.

### Thread-local frame mechanics

- `stdc_raise` records the frame whose decider is about to run in a per-thread
  in-flight slot before calling it. The innermost frame is not necessarily the
  one being consulted (an inner frame that does not guard this signo is skipped
  at `:322-324`), so the slot — not `tss->front` — identifies the target.
- `abandon` from normal (non-raise) context targets `tss->front`; from a decider
  it targets the in-flight frame. Unlink splices the frame out wherever it is
  (`prev_elem->prev = frame->prev`, or `front = frame->prev` if head); a
  reference to the parked frame is recorded in the caller's
  `sigguarded_abandoned_state_t`. Pointer-only work — no malloc, no lock.
- `resume` re-links at the **head of the current chain**
  (`frame->prev = tss->front; tss->front = frame`). `frame->prev` at abandon
  time is unreliable (ancestors may have popped and been recycled while parked),
  so it is not used. Re-linking at the head is correct whenever no live
  *descendant* guard exists at resume time; a descendant pushed after the
  abandon and still live would invert the order, so the contract requires
  `resume` before the abandoned context creates any new nested guard. This
  restriction is load-bearing (see the mid-chain analysis below).
- **Mid-chain safety of the epilogue/recovery pop.** The base design's
  `tss->front = f->prev` pop is correct for a *linked* frame. If the frame was
  parked (mid-chain, an inner frame `X` still above it), the normal epilogue
  `front = f->prev` would clobber a still-live `X`. Under the
  resume-before-nesting restriction `X` cannot be live at the epilogue, so
  `front == f->prev` and the pop is a no-op that stays correct; to be defensive
  regardless, make both pops conditional: `if(tss->front == f) tss->front = f->prev;`.
  The recovery branch after an in-the-same-call `abandon` + `invoke_recovery`
  needs no special handling: the `longjmp` unwinds every descendant of the frame
  (including any inner `X`), so setting `front = f->prev` correctly drops them.
- **Never-resumed frames** stay parked (bounded leak) and are reclaimed by the
  thread-exit teardown of the base design — same policy as silently abandoned
  frames.

### Global decider mechanics (partial `LEAK`/`NDEC` coverage)

- `stdc_raise`'s global-decider section (`thrd_signal_handle_posix.c.ipp:371-403`)
  runs the decider with `state->lock` released (`:379`) and holds
  `node->refcount++` and `item->lifetime_refcount++`. A never-returning global
  decider leaks both (the `LEAK` finding). The in-flight raise record extends to
  `{item, node}` around this call.
- `abandon` from a global decider only **parks** the `{item, node}` in the
  caller-provided state object and marks the in-flight record abandoned — no
  lock, no refcount touch, so it is async-signal-safe. The refcount release is
  then performed by a
  deferred drain: the next library entry on the thread (or thread-exit
  teardown) sees the parked record, takes `state->lock` in normal context, and
  replicates the post-decider bookkeeping (`--node->refcount`, retire to
  `deferred_frees` if zero, `sighandler_info_release(item)`), honouring the
  orphaned-node rules of `AA1`/`AB1`. This is exactly the `LEAK` remedy the
  plan keeps out of scope — the API gives it a trigger.
- **`resume` is only meaningful while the decider call is still live.** After a
  global decider actually transfers control away (its longjmp), the raise
  machinery is gone and there is nothing to re-enter; the honest "undo" for a
  global decider is simply returning — so the post-decider path must treat a
  parked-but-returned decider as if it had not abandoned (clear the parked
  record and run the normal bookkeeping, no double-decrement). Retractable
  `resume` therefore only has full meaning for thread-local guards; the global
  form reduces to `abandon` + implicit undo-on-return.

### Async-signal-safety analysis

- Thread-local form: pure per-thread pointer/flag operations plus the existing
  `atomic_signal_fence` discipline — async-signal-safe on the async-safe TLS
  path. On the fallback TLS path `sig_global_tss_state()` takes the TSS spinlock
  (the `SPIN` finding), so the API's async-signal-safety is contingent on
  `plans/ideas.md` item 1 (lock-free cached per-thread TSS value); since the
  caller is already inside a guard, the TSS is already initialised and a cached
  TLS pointer suffices.
- Global form: `abandon` records intent only (async-safe); the drain runs in
  normal context later. This relies on `stdc_raise` releasing `state->lock`
  before invoking global deciders, which is already true at `:379`.

### Edge cases / contract restrictions (summary)

1. `abandon` outside any guard/decider context → nonzero return, `*state`
   untouched.
2. `abandon` with a null `state` → permissible; no retraction information
   stored; the declaration is irrevocable.
3. Double `abandon` in the same context → both calls store valid retraction
   info (the frame within which the calls are made is already parked); the
   first `resume` retracts, later `resume`s with the other info return nonzero.
4. `resume` with a null / never-filled / foreign-thread / modified state object
   → UB (jmp_buf-class, like a `jmp_buf` not set by `setjmp`); with valid but
   already-retracted info → nonzero, no effect.
5. `abandon` then real termination (longjmp out, `_exit`) without `resume` →
   parked frame/record reclaimed at thread-exit teardown (bounded leak); the
   filled-in state object's contents become stale.
6. `resume` after real termination → UB (frame memory may be recycled).
7. `resume` must precede any new nested guard created by the abandoned context.
8. `abandon` + `invoke_recovery` in the same decider call: analysed, safe with
   heap frames (the longjmp unwinds descendants; `front = f->prev` is correct).
9. Nested raises (`SA_NODEFER`) overwrite the in-flight record — it must be
   save/restored per raise nesting (`NSTR` sibling; note the interaction).

### Comparison with the base design

| Aspect | Base design (sound-skip + heap frames) | `sigguarded_abandon`/`sigguarded_abandon_resume` |
|---|---|---|
| Uncooperative never-returning decider | Handled (heap frames + address skip) | Not handled — needs the base machinery too, or a stronger contract |
| Residual (research conclusion 4) | Stale frame may still be consulted; recovery through it UB | Eliminated for cooperative code |
| Retractable (undo opt-out) | Impossible | Native |
| New public API | None | Two functions + implementation-defined `sigguarded_abandoned_state_t` |
| Per-frame / per-thread cost | +1 pointer (`home_mark`) | +1 `linked` flag, in-flight raise record |
| Async-signal-safe | Yes (pure compares) | Yes for thread-local form on async-safe TLS; fallback needs ideas.md item 1 |
| Findings fixed | `JLGS` (POSIX) + co-fix `SJMP` | Cooperative part of `JLGS`; hooks `LEAK`/`NDEC` drain |
| Test surface | 1 new test file | 1 new test file + API/contract tests |

### Recommendation

Keep the base design as the primary fix: it is the only thing that makes the
*uncooperative* never-returning decider safe, which N3924 requires. Treat the
two new APIs as a complementary extension that builds on the same heap-frame
restructure: (a) eliminates the skip residual for code that opts in, (b) adds
the retract capability, and (c) gives the `LEAK`/`NDEC` deferred drain a public
trigger. The additions are small (`linked` flag, in-flight record, two exported
functions); the global-decider drain should stay with the `LEAK` plan, scoping
the new APIs' normative core to thread-local guards with the "usually
async-signal-safe" claim. If the APIs are not adopted, record the option in
`plans/ideas.md` so the residual is a known, deliberate boundary rather than an
unconsidered one.

## Design

Per-thread guard frames allocated from the per-thread TSS of the thread (never
the guard's own stack):

- `sigguarded` takes a frame from per-thread storage (a small per-thread
  freelist; on first use/overflow it `calloc`s — normal context only, matching
  the documented "usually async-signal-safe after one library call per thread"
  contract), records `frame->home_mark = (uintptr_t)&local_probe` (address of
  a `volatile` local in `sigguarded` = the guard's own stack position),
  chains it (`frame->prev = tss->front`), calls `setjmp(frame->buf)`, and
  *then* publishes `tss->front = frame`. Normal return and the
  `setjmp != 0` recovery branch both unlink the frame and recycle/keep it.
- `stdc_raise` takes a `volatile` local `sp_probe`, and while walking the chain
  skips any frame with `(uintptr_t)frame->home_mark <= (uintptr_t)&sp_probe`
  (provably dead: its `sigguarded` is not an ancestor of the raise), then
  continues with `frame = frame->prev`. All existing decider logic
  (next_decider / resume_execution / invoke_recovery) is unchanged for frames
  that pass the test.
- Abandoned frames (never popped) stay allocated (bounded leak per abandoned
  guard) and are freed at thread-exit teardown of the per-thread TSS.
- No changes to `raw_signal_handler`, `invoke_sigaction`, or the global
  decider/install machinery. `stdc_raise` stays allocation-free and
  async-signal-safe (the skip test is pure address compares on memory).

### Frame storage & lifecycle

- Add member to `sig_global_state_tss_state_t`
  (`thrd_signal_handle_common.ipp.ipp:267-282`), under `#ifndef _WIN32`:
  `struct sig_global_state_tss_state_per_frame_t *frame_freelist;` (recycled
  and abandoned frames; the chain head remains `front`).
- Add `void *home_mark;` to `sig_global_state_tss_state_per_frame_t`
  (`thrd_signal_handle_common.ipp.ipp:256-266`) — shared struct, unused (NULL)
  on Windows, which doesn't walk it.
- `sig_global_state_tss_state_per_frame_t` is currently ~280 bytes
  (`jmp_buf` dominated). A frame is created on first use and recycled on pop,
  so steady-state sigguarded cost is one freelist pop/push (no malloc).

### Thread-exit teardown (free strays)

- async-safe-TLS path: `sig_global_tss_state_init` currently registers
  `thread_atexit(free, mem)` (`:296-317`). Replace with a static
  `void sig_global_tss_state_atexit(void *p)` that walks `mem->front`
  (abandoned/live-at-exit chain frames) and `mem->frame_freelist`, freeing each
  frame, then `free(mem)`. Registered as `thread_atexit(sig_global_tss_state_atexit, mem)`
  (signature matches, no fn-pointer type-pun).
- fallback-TLS path: `sig_global_state_tss_state_destroy(void *p)`
  (`:341-345`) does the same walk-then-free before `free(p)`. (This function
  already runs under the last `siguninstall` while other threads may be inside
  `sigguarded` — that is the pre-existing `UNTL` defect, unchanged by this plan;
  addressable separately.)

### `sigguarded` (POSIX, `thrd_signal_handle_posix.c.ipp:241-287`)

Replace the stack-local frame with the pooled heap frame:

```c
struct tss_state_per_frame_t *f = freelist_pop(tss);      /* or calloc */
if(f == NULL) { /* set + return { .int_value = -1 } like init failure */ }
volatile char home_probe;                     /* guard's own stack position */
f->home_mark = (uintptr_t) &home_probe;
memset-into-f the prev/guarded/recovery/decider/rsi.value fields (as today);
/* SJMP fix: publish AFTER setjmp */
if(SETJMP(f->buf) != 0) {
  tss->front = f->prev;
  atomic_signal_fence(...);   /* prev handler active before recovery may raise */
  ret = recovery(&f->rsi);    /* frame already unlinked: recovery must return */
  freelist_push(tss, f);
  return ret;
}
atomic_signal_fence(...);
tss->front = f;                               /* published after setjmp returned 0 */
ret = guarded(value);                         /* guarded must return (contract) */
tss->front = f->prev;
atomic_signal_fence(...);
freelist_push(tss, f);
return ret;
```

Notes:
- `tss->front = old` is now `tss->front = f->prev`; the `prev` field replaces
  the saved `old`.
- `memset(f, 0, sizeof(*f))` before fill (frames are recycled).
- Abandonment (a never-returning decider) leaves `f` linked and off the
  freelist → stays allocated, freed at thread-exit teardown.
- NULL/`fail` args still `abort()` (unchanged, AGENTS.md rule 9). Frame
  allocation failure mirrors the existing `sig_global_tss_state_init` failure
  return (`AMBI` note: -1 remains ambiguous — document, don't fix here).

### `stdc_raise` frame walk (POSIX, `thrd_signal_handle_posix.c.ipp:318-348`)

Inside the walk, before consulting a frame:

```c
volatile char sp_probe;                       /* this raise's stack position */
...
while(frame != NULL) {
  if((uintptr_t) frame->home_mark <= (uintptr_t) &sp_probe) {
    frame = frame->prev;                      /* provably dead: skip outward */
    continue;
  }
  if(sigismember(frame->guarded, signo)) { ... existing decider switch ... }
  frame = frame->prev;
}
```

This is pure address arithmetic — allocation-free and async-signal-safe; the
skip never drops a live frame (live frames are always strict ancestors).

### Documentation

- `thrd_signal_handle.h` `sigguarded` doc comment (`:499-525`): state the
  return contract — `guarded` and `recovery` MUST return; if either never
  returns the behaviour is undefined (the user's own `setjmp`/`longjmp`-class
  misuse; `_Exit`/`_exit` inside them ends the process, so no state persists).
  The `decider` IS permitted to never return (N3924 7.14.1); the library keeps
  frames in per-thread storage (no reused-stack access ever) and the raise path
  silently passes over guard frames that are no longer on the calling stack.
  Requesting recovery (`sig_decision_invoke_recovery`) through an abandoned
  guard is undefined behaviour.
- `Readme.md` "Known issues and limitations": a matching bullet.
- `docs/proposed-wording.md` is vendored WG14 wording — do not edit except the
  draft `sigguarded_abandon`/`sigguarded_abandon_resume` extension
  (`7.14.4.2`-`7.14.4.3` + 7.14.1 references) added 2026-08-16 at the author's
  request, which awaits review. Note (analysis.md-style) that the wording should
  get a WG14 clarification sentence.

### Out of scope / related findings (do NOT touch in this change)

- `SJMP` (row 3): publish ordering fixed here as a co-change; keep the `SJMP`
  heading open with a "partially addressed in JLGS fix" note, or mark reduced.
- `NSTR` (row 4): shared `frame->rsi` re-entrancy — unaffected, separate.
- `NDEC`/`LEAK` (rows 5/11): never-returning *global* deciders — different
  machinery (Windows raise-frame pinning; POSIX refcount/container leak),
  separate. The per-thread "in-flight raise" guard ideas there are compatible
  but not part of this fix.
- `UNTL` (row 2): TSS freed under a live guard — unchanged.
- `TIDR` (row 22): heap frames are per-thread TSS-owned, so a stale-TID reuse
  still mis-keys — unchanged.
- Frame-exhaustion policy: abandoned guards leak one frame until thread exit;
  repeated abandonment grows the freelist/chain — bounded by the number of
  abandons, reclaimed at exit. Document in the header note. No hard cap needed
  (frames are `calloc`'d, not pool-bound).

## Test plan (new `test/guard_abandon_test.c`, via `add_code_test`)

Run on all CI legs (works under Fil-C: no real fault needed, use
`SIGUSR1`-style signal as `thrd_signal_handle_test.c` does; single-threaded →
no AGENTS.md rule 5 concerns).

1. **Decider abandons then raise (the confirmed repro):** `main` does
   `setjmp(env)`; `guarded` calls `stdc_raise`; the frame decider does
   `longjmp(env, 1)` past `sigguarded` and never returns (permitted: N3924
   7.14.1). After the longjmp the thread calls
   `stdc_raise(SIGNAL_TO_USE, NULL, NULL)` again.
   Assert: no crash/ASan report, the abandoned frame's decider is **not** called
   again (`decider_calls == 1`), `stdc_raise` returns `false` (no other
   deciders). Pre-fix: ASan `stack-use-after-scope`; post-fix: clean + assert
   passes.
2. **Abandon then fresh guard:** after decider abandonment, a *new* `sigguarded`
   with a claiming decider + recovery works normally (`decider_calls == 2`,
   `recovery_calls == 1`, recovered value correct) — proves the repair leaves
   the machinery functional and the stale frame did not corrupt the chain.
3. **Nested abandon with live outer (optional, deterministic):** inner
   `sigguarded` inside outer `guarded()`; the inner frame's decider longjmps to
   an env inside the outer `guarded` body; then `stdc_raise` → inner decider
   not called again, outer decider + recovery called exactly once. Validates
   skip-and-continue-outward geometry.
4. **Normal return paths unchanged (regression for the corrected contract):**
   (a) `guarded` returns normally → a subsequent raise does not see the guard
   (`decider_calls` unchanged, `stdc_raise` false); (b) a recovery path returns
   normally with the chain intact. The existing
   `thrd_signal_handle_test`/`recovery_null_loop_test` coverage already guards
   this; assert the abandoned-frame machinery did not alter either path.

`test/CMakeLists.txt`: one `add_code_test(guard_abandon_test ...)` entry.

## Validation

Per AGENTS.md rule 3, follow `.github/workflows/ci.yml` for the host (macOS,
arm64):

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_STANDARD=11 \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/../cmake/sanitize-toolchain.cmake
cmake --build . --parallel
ctest --output-on-failure --timeout 300 -E benchmark
```

Required checks:

- New `guard_abandon_test` passes Debug+Release, C11+C23, shared ON/OFF,
  sanitize (ASan: proves the stack-use-after-scope is gone), and the TSan leg
  (no new races).
- Full ctest: all existing guard/recovery tests pass
  (`thrd_signal_handle_test`, `recovery_null_loop_test`,
  `sigguarded_tss_init_test`, `post_uninstall_reentry_test`,
  `decider_*_test`, header-only C/C++ consumers, install consumer).
- `clang-format` on every changed header and source file (AGENTS.md rule 2;
  do not format cmake files). C11-only code under `include/` and `src/`; no C++
  additions outside `test/`.
- Explicitly probe the async-safe-TLS path (Linux) and the fallback path
  (macOS native, and `-DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON`) — both must
  pass teardown and the abandon test, since frame lifecycle differs only in the
  teardown routine.

## Documentation updates (AGENTS.md rule 6)

After implementation, update `plans/analysis.md`:

- `JLGS` heading (`### 1`): mark fixed — append `[fixed ...]` to the heading or
  a "**Fixed:**" paragraph recording the change location (POSIX `sigguarded` +
  `stdc_raise` frame walk + per-thread frame freelist in
  `thrd_signal_handle_common.ipp.ipp`) and verification (new
  `guard_abandon_test`, ASan clean, all CI legs), and re-scope the residual to
  the documented return-contract boundary.
- §4 tier-2 rationale and the §4/§5 summary tables: update the `JLGS` row to
  reflect it is fixed (and that `SJMP` publish-order is co-fixed there).
- `plans/ideas.md`: optionally record the "sound-skip + stable frame storage"
  technique under §1 implementation techniques.

## Risks / open decisions

- **Skew/optimization:** the `home_mark` and `sp_probe` locals must be
  `volatile` and their addresses taken so the compiler cannot place them in
  registers or elide them; the strict `>` vs `<=` margin absorbs any frame-size
  variance (address-based, ABI-independent).
- **Alternative stack (SA_ONSTACK) is not used** by `install_sighandler_impl`
  (`thrd_signal_handle_posix.c.ipp:412-424`), so raise probes and guard frames
  share the thread's main stack; if SA_ONSTACK is ever added, the probe check
  would need a stack-identity adjustment. Document the assumption.
- **Freelist vs malloc-per-guard:** freelist chosen to keep `sigguarded`
  alloc-free in steady state (preserves the "usually async-signal-safe" claim);
  malloc fallback only on first use. If simplicity is preferred over that claim,
  malloc-per-guard is acceptable with the same semantics.
- **Return contract:** `guarded`/`recovery` never returning is now documented
  UB, not a supported mode; no defensive machinery is added for it beyond the
  heap-frame safety that the never-returning-decider case already requires.
- **Explicit abandon/resume APIs (researched 2026-08-16):** a full alternative
  design — two new public APIs (`sigguarded_abandon` /
  `sigguarded_abandon_resume`) making abandonment cooperative and retractable —
  is documented in the "Alternative design" section above, with draft normative
  wording in `docs/proposed-wording.md` (`7.14.4.2`-`7.14.4.3`, awaiting
  review). It does not replace this plan (it cannot fix the uncooperative
  decider N3924 permits) and is deliberately not implemented here; the `linked`
  flag, in-flight raise record, and `if(front == f)` pop are cheap enough to
  add later on top of the same heap-frame restructure if the extension is
  adopted.
- **Frame-field count:** `home_mark` adds one pointer per frame; on Windows the
  field is unused (calloc'd NULL) — verified no Windows walker reads it.
