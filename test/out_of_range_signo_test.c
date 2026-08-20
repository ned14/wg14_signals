#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// The regression must guard the signal a negative signo aliases to on the
// shift-count sigset platforms: stdc_raise(-1)'s sigismember shift wraps to
// bit 30, i.e. signal 31 = SIGUSR2 on macOS/FreeBSD (plans/analysis.md NEGS,
// probe-verified). SIGUSR2 is not defined on MSVC; there -- and on glibc, whose
// sigismember returns 0 out of range -- the bug was invisible anyway, so any
// guarded signal works. SIGABRT would not: on Windows it maps to
// EXCEPTION_NONCONTINUABLE_EXCEPTION, which cannot be resumed (finding `SABA`),
// so a frame decider claiming it with resume_execution loops; SIGILL
// (EXCEPTION_ILLEGAL_INSTRUCTION) is continuable and MSVC-defined.
#ifdef SIGUSR2
#define GUARDED_SIGNAL SIGUSR2
#else
#define GUARDED_SIGNAL SIGILL
#endif

static int frame_decider_calls = 0;
static int sanity_claimed = 0;
static int neg_returned_false = 0;
static int pos_returned_false = 0;
static int far_returned_false = 0;

static enum WG14_SIGNALS_PREFIX(sig_decision)
frame_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  frame_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // Sanity: a real raise of the guarded signal is claimed by the frame decider.
  sanity_claimed = WG14_SIGNALS_PREFIX(stdc_raise)(
  GUARDED_SIGNAL, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  frame_decider_calls = 0;
  // NEGS: out-of-range and negative signos must be rejected before the POSIX
  // frame walk. Pre-fix on macOS/BSD, sigismember()'s shift-count wrap made
  // stdc_raise(-1) invoke this frame's decider with rsi->signo == -1 (aliasing
  // signal 31 = SIGUSR2), so all three raises must return false -- the
  // documented "no decider installed for that signal" -- WITHOUT calling the
  // frame decider.
  neg_returned_false = !WG14_SIGNALS_PREFIX(stdc_raise)(
  -1, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  pos_returned_false = !WG14_SIGNALS_PREFIX(stdc_raise)(
  NSIG, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  far_returned_false = !WG14_SIGNALS_PREFIX(stdc_raise)(
  NSIG + 10, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  return value;
}

int main(void)
{
  int ret = 0;

  // The documented one-line library setup call.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR) == false);

  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, GUARDED_SIGNAL);
  // On Windows the unclaimed-raise machinery (the unhandled exception filter
  // and vectored continue handler that let an unclaimed stdc_raise() return
  // false instead of reaching Windows Error Reporting) exists only after
  // siginstall() (plans/analysis.md 2.16/W5). The sanity raise below is
  // claimed by the frame decider, so it works without it, but the
  // out-of-range raises would terminate the process. Install the guarded
  // signal so the raise-initiated detection is active; harmless on POSIX,
  // where the frame walk needs no kernel handler.
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  (void) WG14_SIGNALS_PREFIX(sigguarded)(&guarded, guarded_func, noop_recovery,
                                         frame_decider, value);
  CHECK(sanity_claimed);
  CHECK(neg_returned_false);
  CHECK(pos_returned_false);
  CHECK(far_returned_false);
  CHECK(frame_decider_calls == 0);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  printf("out-of-range signo rejection checks passed\n");
  return ret;
}
