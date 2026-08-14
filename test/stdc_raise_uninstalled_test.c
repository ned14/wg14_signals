#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGILL is fine everywhere else
// (SEH-mapped to EXCEPTION_ILLEGAL_INSTRUCTION on Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

#define OTHER_SIGNAL SIGABRT

static int guarded_raise_returned_false = 0;

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
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

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // Inside a sigguarded() frame guarding a DIFFERENT signal, a raise of an
  // uninstalled signal must return false too, not terminate the process
  // (analysis.md 2.16/W5).
  guarded_raise_returned_false =
  (WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                   WG14_SIGNALS_NULLPTR) == false);
  return value;
}

int main(void)
{
  int ret = 0;

  (void) WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);

  // Register the library's exception machinery before raising anything: on
  // Windows the vectored continue handler + unhandled exception filter exist
  // only after siginstall(), so a raise raised before it cannot be resolved
  // and Windows Error Reporting terminates the process (analysis.md 2.16/W5,
  // corrected 2026-08-14). Install only OTHER_SIGNAL so SIGNAL_TO_USE stays
  // "no handler installed" for the checks below.
  sigset_t base;
  sigemptyset(&base);
  sigaddset(&base, OTHER_SIGNAL);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&base);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);

  // A supported signo with NO handler installed: stdc_raise must return false
  // (documented contract "returning false if we have no decider installed for
  // that signal"). Before analysis.md 2.16/W5 was fixed, Windows raised the
  // SEH exception, no handler/decider claimed it, and Windows Error Reporting
  // terminated the process.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR) == false);

  // Same raise inside a sigguarded() frame guarding a different signal.
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, OTHER_SIGNAL);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  (void) WG14_SIGNALS_PREFIX(sigguarded)(&guarded, guarded_func, noop_recovery,
                                         claiming_decider, value);
  CHECK(guarded_raise_returned_false);

  // Sanity: once SIGNAL_TO_USE is installed and claimed, stdc_raise must still
  // return true.
  sigset_t sanity;
  sigemptyset(&sanity);
  sigaddset(&sanity, SIGNAL_TO_USE);
  void *handlers2 = WG14_SIGNALS_PREFIX(siginstall)(&sanity);
  CHECK(handlers2 != WG14_SIGNALS_NULLPTR);
  {
    sigset_t guarded2;
    sigemptyset(&guarded2);
    sigaddset(&guarded2, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded2, false, claiming_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    WG14_SIGNALS_PREFIX(signal_decider_destroy(decider));
  }
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers2) == 0);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
