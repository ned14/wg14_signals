#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <setjmp.h>

// Regression test for the sigdecider_abandon()/sigdecider_abandon_resume()
// pair, the current public API (thrd_signal_handle.h) behind draft wording
// 7.14.4.2-7.14.4.3 (docs/proposed-wording.md): a decider -- thread-local or
// global -- which determines that it will never return through the signal
// decider machinery declares that with sigdecider_abandon() before not
// returning, so the machinery can drop the in-flight state it was holding; if
// the decider later determines that it will return after all, the same decider
// call retracts the declaration with sigdecider_abandon_resume() (resuming
// after the decider call has exited is undefined behaviour per the draft
// wording). The signal used is never installed in the POSIX-only parts and
// only installed around the global-decider parts, so an unclaimed raise simply
// returns false instead of reaching any previously installed handler.
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGUSR1
#endif

static int global_decider_calls = 0;

// Abandon then immediately resume within the same global decider call: the
// declaration is retracted before the decider returns, so the raise completes
// and its post-decider bookkeeping runs exactly as if neither call had been
// made.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
global_abandon_resume_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  global_decider_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(rsi);
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int global_abandon_calls = 0;
static jmp_buf global_abandon_env;

// The motivating pattern the draft wording describes (7.14.1): the decider
// determines it will never return through the decider machinery (it longjmps
// to a caller-owned buffer), declares exactly that with sigdecider_abandon(),
// and then never returns. The raise machinery is left at exactly steady state,
// so later raises on the thread consult the decider again.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
global_abandon_never_returns_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) *
                                     rsi)
{
  global_abandon_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  longjmp(global_abandon_env, 1);
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int frame_decider_calls = 0;

// Abandon then immediately resume within the same thread-local frame decider
// call: the frame must remain consultable both by this raise's continued
// processing and by later raises inside the same guard.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
flicker_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  frame_decider_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(rsi);
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int raise_result1 = 0;
static int raise_result2 = 0;

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_calls_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  raise_result1 = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  raise_result2 = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  return value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
single_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // The raise never returns when the decider abandons or recovers.
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

static int raise_result5 = 0;

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
double_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // Only the first raise returns (resume_execution); the second raise's decider
  // invokes recovery and never returns here.
  raise_result5 = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

static int frame_abandon_calls = 0;
static jmp_buf frame_abandon_env;

// The documented never-returning form for a thread-local decider: declare the
// abandonment with sigdecider_abandon() so the machinery pops this frame, then
// never return (here by longjmping to a caller-owned buffer outside the guard).
// The raise machinery is left at exactly steady state.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
frame_abandon_never_returns_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) *
                                    rsi)
{
  frame_abandon_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  longjmp(frame_abandon_env, 1);
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int fresh_decider_calls = 0;
static int fresh_recovery_calls = 0;

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
fresh_claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  fresh_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
fresh_counting_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  fresh_recovery_calls++;
  return rsi->value;
}

static int flicker_recover_calls = 0;

// Abandon twice and resume twice (the repeats must be no-ops), then on the
// second call claim via recovery: a resumed frame must still longjmp through
// its intact buffer.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
flicker_then_recover_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  flicker_recover_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(rsi);
  WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(rsi);
  if(flicker_recover_calls >= 2)
  {
    return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery);
  }
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int nested_inner_abandon_calls = 0;
static int outer_decider_calls = 0;
static int outer_recovery_calls = 0;
static int inner_abandon_failed = 0;
static jmp_buf nested_abandon_env;

// The inner (topmost) guard's decider abandons the inner frame and longjmps to
// an environment inside the outer guard's guarded body, which is still live.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
nested_inner_abandon_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  nested_inner_abandon_calls++;
  WG14_SIGNALS_PREFIX(sigdecider_abandon)(rsi);
  longjmp(nested_abandon_env, 1);
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
outer_claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  outer_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
outer_counting_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  outer_recovery_calls++;
  return rsi->value;
}

// Runs one raise inside an inner guard; the inner decider abandons and longjmps
// to nested_abandon_env before this function can return normally.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
nested_inner_guard(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  sigset_t inner;
  sigemptyset(&inner);
  sigaddset(&inner, SIGNAL_TO_USE);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) inner_value = {.int_value = 7};
  (void) WG14_SIGNALS_PREFIX(sigguarded)(&inner, single_raise, noop_recovery,
                                         nested_inner_abandon_decider,
                                         inner_value);
  return value;
}

// The outer guard's guarded body: raise inside an inner guard whose decider
// abandons and longjmps back here, then raise again so the still-live outer
// frame handles it.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
outer_guarded_body(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  if(setjmp(nested_abandon_env) == 0)
  {
    nested_inner_guard(value);
    inner_abandon_failed = 1;
  }
  // The outer decider recovers, so this invokes the outer recovery and does not
  // return here.
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

int main(void)
{
  // static: the never-returning decider longjmps back into main, and C11
  // 7.13.2.1 makes non-volatile automatic objects modified between setjmp and
  // longjmp indeterminate; a static accumulator survives the jump, so the
  // failure count is preserved on the longjmp paths.
  static int ret = 0;

  SECTION("Global decider abandons and resumes in-call: the raise completes "
          "and the decider runs again on the next raise");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers != WG14_SIGNALS_NULLPTR);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *decider1 = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, global_abandon_resume_decider, value);
    CHECK(decider1 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(global_decider_calls == 1);
    // No contamination from the first raise: the second raise runs the decider
    // again with a fresh in-flight record.
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(global_decider_calls == 2);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider1) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  }

  SECTION("Global decider abandons then never returns: the machinery is left "
          "at steady state and consults the decider again");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers != WG14_SIGNALS_NULLPTR);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, global_abandon_never_returns_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    if(setjmp(global_abandon_env) == 0)
    {
      // Only the failure path continues past the raise: a successful abandon
      // longjmps back before stdc_raise returns, so the marker below runs only
      // when the decider failed to abandon (it then claims the raise by
      // resume_execution instead of longjmping back).
      CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                            WG14_SIGNALS_NULLPTR));
      CHECK(0 && "global decider failed to abandon");
    }
    CHECK(global_abandon_calls == 1);
    // A fresh raise on the thread consults the decider again and is abandoned
    // again: nothing from the abandoned raise stuck.
    if(setjmp(global_abandon_env) == 0)
    {
      CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                            WG14_SIGNALS_NULLPTR));
      CHECK(0 && "global decider failed to abandon");
    }
    CHECK(global_abandon_calls == 2);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  }

#ifndef _WIN32
  // The POSIX-specific arms below exercise the thread-local guard frame chain.
  // On Windows sigguarded() is implemented with structured exception handling
  // and installs no thread-local decider frame, so there is no frame for a
  // thread-local decider to abandon there (documented in thrd_signal_handle.h
  // and draft wording 7.14.1). The signal is deliberately never installed in
  // these arms, so an unclaimed raise returns false instead of reaching any
  // previously installed handler. The in-call abandon + resume form is
  // exercised here: a decider which determines it will return after all
  // retracts its abandonment within the same decider call, and the frame must
  // stay consulted by later raises inside the same guard. The never-returning
  // forms -- with a cooperative sigdecider_abandon() -- are exercised in the
  // sections below; a decider which never returns WITHOUT abandoning is the
  // documented residual (requesting recovery through it is undefined
  // behaviour).
  SECTION("Frame decider abandons and resumes in-call: the guard stays "
          "consultable and pops normally on return");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    const int before = frame_decider_calls;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) guarded_ret =
    WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, guarded_calls_raise, noop_recovery, flicker_decider,
    (union WG14_SIGNALS_PREFIX(stdc_siginfo_value)) {.int_value = 7});
    // The in-call abandon + resume left the frame linked and consulted by the
    // second raise inside the same guard, and the guarded value round-trips.
    CHECK(raise_result1);
    CHECK(raise_result2);
    CHECK(guarded_ret.int_value == 7);
    CHECK(frame_decider_calls == before + 2);
    // The retracted frame was popped by the normal return path: nothing is
    // consulted anymore.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    CHECK(frame_decider_calls == before + 2);
  }

  SECTION("Frame decider abandons then never returns: the popped frame is not "
          "consulted and a fresh guard still recovers");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    frame_abandon_calls = 0;
    fresh_decider_calls = 0;
    fresh_recovery_calls = 0;
    if(setjmp(frame_abandon_env) == 0)
    {
      // Only the failure path continues past the raise: a successful abandon
      // longjmps back before stdc_raise returns, so the marker below runs only
      // when the decider failed to abandon (it then claims the raise by
      // resume_execution instead of longjmping back).
      union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
      WG14_SIGNALS_PREFIX(sigguarded)(&guarded, single_raise, noop_recovery,
                                      frame_abandon_never_returns_decider,
                                      value);
      CHECK(0 && "frame decider failed to abandon");
    }
    CHECK(frame_abandon_calls == 1);
    // The abandoning decider popped the frame before longjmping, so nothing is
    // consulted anymore; the signal is never installed in this arm, so the
    // unclaimed raise returns false.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    CHECK(frame_abandon_calls == 1);
    // A fresh guard on the same thread works normally: its decider claims the
    // raise exactly once, the recovery runs, and the guarded value round-trips
    // through the recovered return.
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret_value =
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, single_raise,
                                    fresh_counting_recovery,
                                    fresh_claiming_decider, value);
    CHECK(fresh_decider_calls == 1);
    CHECK(fresh_recovery_calls == 1);
    CHECK(ret_value.int_value == 7);
  }

  SECTION("Abandon and resume is idempotent and a resumed frame still recovers "
          "by longjmp");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    flicker_recover_calls = 0;
    raise_result5 = 0;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret_value =
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, double_raise, noop_recovery,
                                    flicker_then_recover_decider, value);
    // First raise: the abandoned-then-resumed decider returns resume_execution.
    // Second raise: the resume restored the frame's longjmp buffer, so
    // invoke_recovery lands in sigguarded's recovery branch, which pops the
    // frame and returns the recovered value.
    CHECK(raise_result5);
    CHECK(flicker_recover_calls == 2);
    CHECK(ret_value.int_value == 7);
    // The recovery path popped the frame: nothing is consulted anymore.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    CHECK(flicker_recover_calls == 2);
  }

  SECTION("Nested abandon with a live outer guard: the inner frame is dropped, "
          "the outer guard still recovers");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    nested_inner_abandon_calls = 0;
    outer_decider_calls = 0;
    outer_recovery_calls = 0;
    inner_abandon_failed = 0;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret_value =
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, outer_guarded_body,
                                    outer_counting_recovery,
                                    outer_claiming_decider, value);
    // The inner raise was abandoned: the inner decider ran exactly once and the
    // post-recovery raise consulted only the still-live outer frame.
    CHECK(!inner_abandon_failed);
    CHECK(nested_inner_abandon_calls == 1);
    CHECK(outer_decider_calls == 1);
    CHECK(outer_recovery_calls == 1);
    CHECK(ret_value.int_value == 7);
    // The outer recovery path popped the frame: nothing is consulted anymore.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    CHECK(outer_decider_calls == 1);
  }
#endif

  return ret;
}
