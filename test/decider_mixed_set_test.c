#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// The mixed guarded set must contain a non-installed signal that is
// numerically *smaller* than the installed signal, otherwise the 1.2 slot
// misalignment cannot occur and the regression is not exercised.
#if defined(_WIN32)
#define INSTALLED_SIGNAL SIGTERM
#define NONINSTALLED_SIGNAL SIGINT
#else
#define INSTALLED_SIGNAL SIGUSR2
#define NONINSTALLED_SIGNAL SIGUSR1
#endif

static volatile sig_atomic_t old_handler_calls = 0;
static void old_signal_handler(int signo)
{
  (void) signo;
  old_handler_calls++;
}

static enum WG14_SIGNALS_PREFIX(sig_decision)
decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_call_recovery);
}

int main(void)
{
  int ret = 0;
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {0};

  // Give the installed signal a benign old handler so that on POSIX a raise
  // after the decider has been destroyed invokes it synchronously instead of
  // terminating the process.
  if(SIG_ERR == signal(INSTALLED_SIGNAL, old_signal_handler))
  {
    fprintf(stderr, "signal() failed: %s\n", strerror(errno));
    return 1;
  }

  for(int cycle = 0; cycle < 10; cycle++)
  {
    sigset_t installed;
    sigemptyset(&installed);
    sigaddset(&installed, INSTALLED_SIGNAL);
    void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&installed);
    CHECK(handlers != WG14_SIGNALS_NULLPTR);
    if(handlers == WG14_SIGNALS_NULLPTR)
    {
      fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
      return ret;
    }

    // Guarded set contains a non-installed signal preceding an installed one.
    // Regression test for analysis.md 1.1 (create must not leak the global
    // spinlock on the warning path) and 1.2 (create and destroy must advance
    // exactly one handle slot per guarded signal).
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, NONINSTALLED_SIGNAL);
    sigaddset(&guarded, INSTALLED_SIGNAL);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, decider_func, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    if(decider == WG14_SIGNALS_NULLPTR)
    {
      WG14_SIGNALS_PREFIX(siguninstall)(handlers);
      return ret;
    }

    // If the 1.1 lock leak returns, this call spins forever and ctest times
    // out; if the 1.2 slot misalignment returns, destroy returns -1.
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider) == 0);

#if !defined(_WIN32)
    // On POSIX a raise with no deciders synchronously invokes the previously
    // installed handler. Before the 1.2 fix this dereferenced the decider node
    // that was freed while still linked into INSTALLED_SIGNAL's
    // global_handler list.
    const volatile sig_atomic_t old_handler_calls_before = old_handler_calls;
    for(int n = 0; n < 100; n++)
    {
      WG14_SIGNALS_PREFIX(stdc_raise)(INSTALLED_SIGNAL, WG14_SIGNALS_NULLPTR,
                                      WG14_SIGNALS_NULLPTR);
    }
    CHECK(old_handler_calls == old_handler_calls_before + 100);
#endif

    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  }

  return ret;
}
