#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGILL is fine everywhere else
// (SEH-mapped to EXCEPTION_ILLEGAL_INSTRUCTION, which is continuable, on
// Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

// Must differ from SIGNAL_TO_USE and be defined on every platform (MSVC does
// not define SIGUSR1/SIGUSR2). It is never raised by this test, only used as
// the guarded set of the sigguarded() frame. SIGABRT would be unsuitable on
// Windows: it maps to EXCEPTION_NONCONTINUABLE_EXCEPTION, which cannot be
// resumed.
#define OTHER_SIGNAL SIGTERM

static enum WG14_SIGNALS_PREFIX(sig_decision)
claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

// The outcome travels through sigguarded()'s value parameter -- the
// documented channel for caller state: sigguarded() hands the value passed
// by the caller to the guarded function verbatim, and returns the guarded
// function's return value unchanged, so no file-scope global is needed to
// observe what happened inside the guard.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  value.int_value =
  (WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                   WG14_SIGNALS_NULLPTR) == false);
  return value;
}

int main(void)
{
  int ret = 0;

  // stdc_raise() of a signal with NO siginstall() ever performed must return
  // false immediately -- the documented "no decider installed for that
  // signal" return -- and must not terminate the process. This is the very
  // first library call in the process. On POSIX the map lookup miss returns
  // false; on Windows the raise never happens because the exception
  // resolution machinery (vectored continue handler + unhandled exception
  // filter) exists only while a handler is installed (analysis.md 2.16/W5),
  // and a raise without it would reach Windows Error Reporting and never
  // return. Before the Windows pre-check, this test died on the Windows CI
  // legs; it now passes everywhere.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR) == false);

  // The documented one-line library setup call must still work and return
  // false.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR) == false);

  // Same raise inside a sigguarded() frame guarding a different signal, with
  // no siginstall() anywhere in the process (analysis.md 2.16/W5 covers the
  // installed-other-signal case; this closes the no-install-at-all gap).
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, OTHER_SIGNAL);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) result = WG14_SIGNALS_PREFIX(
  sigguarded)(&guarded, guarded_func, noop_recovery, claiming_decider, value);
  // guarded_func overwrites the value with "the no-install stdc_raise()
  // returned false", and sigguarded() passes that back out; the -99 failure
  // sentinel can never be confused with it.
  CHECK(result.int_value == 1);

  printf("no-install stdc_raise rejection checks passed\n");
  return ret;
}
