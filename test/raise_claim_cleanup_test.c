// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): the .ipp
// implementations compiled below must see the same declarations as the compiled
// library. _GNU_SOURCE alone is the mirror -- on glibc it subsumes the
// library's _POSIX_C_SOURCE/_XOPEN_SOURCE trio, and macOS/BSD must not take
// those at all (they hide NSIG, which the signo-to-sighandler map needs,
// finding `NSIG`).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <string.h>

// Regression test for analysis.md RFLK: a sigguarded() frame decider claiming a
// stdc_raise()-initiated raise with sig_decision_call_recovery must leave the
// per-thread raise-frame chain empty. On Windows stdc_raise() pushes its raise
// frame and Win-state marker onto tss->front / stdc_raise_initiated_exception
// before RaiseException() and pops them only on the unclaimed-return and
// longjmp-back paths; the frame filter's EXECUTE_HANDLER path makes SEH unwind
// stdc_raise()'s stack frame, so those pop paths never run and both chain
// heads dangle at the dead frame -- a later raise re-links onto the dead
// pointers, a genuine fault claimed by a global decider longjmps into the dead
// environment, and sigdecider_abandon() dereferences the dead frame. The
// POSIX equivalent is clean (the frame claim longjmps into the frame's own
// sigguarded(), which restores tss->front), so the white-box chain checks below
// pass on both platforms and fail on Windows before the fix.
//
// Header-only mode (WG14_SIGNALS_ENABLE_HEADER_ONLY) makes the internal
// sig_global_tss_state() and sig_global_state_tss_state_t callable from this
// TU so the test can inspect the chain (the header_only_c_multi_test pattern,
// plans/ideas.md 10).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

static int recovery_calls = 0;

static enum WG14_SIGNALS_PREFIX(sig_decision)
claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_call_recovery);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
counting_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  recovery_calls++;
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
raise_signal(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // The software raise that the frame decider below claims with call_recovery.
  // On Windows this is the stdc_raise()-initiated SEH exception; on POSIX it
  // walks the frame chain directly.
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

int main(void)
{
  int ret = 0;

  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGNAL_TO_USE);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }

  recovery_calls = 0;
  const union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  const union WG14_SIGNALS_PREFIX(stdc_siginfo_value) result =
  WG14_SIGNALS_PREFIX(sigguarded)(&guarded, raise_signal, counting_recovery,
                                  claiming_decider, value);
  // The frame decider claimed the raise with call_recovery: sigguarded() ran
  // recovery exactly once and returned its value.
  CHECK(recovery_calls == 1);
  CHECK(result.int_value == 7);

  // White-box: the per-thread raise chain must be empty after the claim. On
  // Windows before the fix tss->front still pointed at the unwound stdc_raise()
  // stack frame (analysis.md RFLK).
  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
  WG14_SIGNALS_PREFIX(sig_global_tss_state)();
  CHECK(tss->front == WG14_SIGNALS_NULLPTR);
#ifdef _WIN32
  CHECK(tss->stdc_raise_initiated_exception == WG14_SIGNALS_NULLPTR);
#endif

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  // POSIX-only: a later raise on the thread, with no decider or handler left,
  // must return false without re-linking onto a dead frame (analysis.md RFLK).
#if !defined(_WIN32)
  CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR));
  tss = WG14_SIGNALS_PREFIX(sig_global_tss_state)();
  CHECK(tss->front == WG14_SIGNALS_NULLPTR);
#endif

  return ret;
}
