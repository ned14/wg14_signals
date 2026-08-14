#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Fil-C's runtime forbids user handlers for SIGSEGV (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGSEGV is fine everywhere else
// (SEH-mapped to EXCEPTION_ACCESS_VIOLATION on Windows). Deliberately do NOT
// call stdc_raise(0, ...) anywhere in this test: that setup call initialises
// the calling thread's per-thread TSS, which is exactly the state this test
// must leave uninitialised until the sigguarded() under test (analysis.md
// 2.19/X3).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGSEGV
#endif

static int global_decider_called = 0;

// The thread-local frame decider declines, so the raise propagates past the
// guarded frame to the global decider for the same signal -- the path which
// runs inside the vectored exception function on Windows.
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
declining_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
}

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  global_decider_called++;
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
#ifdef _WIN32
  // A genuine-fault-style exception (EXCEPTION_ACCESS_VIOLATION maps to
  // SIGSEGV). The frame decider declines, so the vectored exception function
  // runs the global decider for SIGSEGV. Before analysis.md 2.19/X3 was fixed,
  // sigguarded() had not initialised this thread's per-thread TSS, and the
  // claiming-decider path dereferenced tss->front on the NULL state -- a crash
  // inside the exception handler.
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, WG14_SIGNALS_NULLPTR);
#else
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
#endif
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
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
  &guarded, false, claiming_decider, value);
  CHECK(decider != WG14_SIGNALS_NULLPTR);

  // This thread's only library interaction is sigguarded(). On Windows that
  // call must set up the per-thread TSS so the vectored exception function can
  // safely evaluate tss->front when a global decider claims the fault; on
  // POSIX it already does (analysis.md 2.19/X3).
  const union WG14_SIGNALS_PREFIX(stdc_siginfo_value) result =
  WG14_SIGNALS_PREFIX(sigguarded)(&guarded, guarded_func, noop_recovery,
                                  declining_decider, value);
  CHECK(global_decider_called == 1);
  CHECK(result.int_value == 7);

  WG14_SIGNALS_PREFIX(signal_decider_destroy(decider));
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
