#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Regression test for plans/analysis.md TCOV: the failure/edge APIs had no
// coverage. siguninstall_system, the asynchronous sigfillset helpers and the
// signal_decider_create error paths are now all exercised here.

static enum WG14_SIGNALS_PREFIX(sig_decision)
never_called_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
}

int main(void)
{
  int ret = 0;

  SECTION("sigfillset_asynchronous_nondebug and sigfillset_asynchronous_debug");
  sigset_t nondebug;
  memset(&nondebug, 0, sizeof(nondebug));
  CHECK(WG14_SIGNALS_PREFIX(sigfillset_asynchronous_nondebug)(&nondebug) == 0);
  // The non-debug asynchronous set must not contain synchronous-fault
  // signals: SIGSEGV is a member of the synchronous set only, on both
  // backends.
  CHECK(sigismember(&nondebug, SIGSEGV) != 1);
  sigset_t debug;
  memset(&debug, 0, sizeof(debug));
  CHECK(WG14_SIGNALS_PREFIX(sigfillset_asynchronous_debug)(&debug) == 0);

  SECTION("siguninstall_system");
  CHECK(WG14_SIGNALS_PREFIX(siguninstall_system)(0) == 0);
  errno = 0;
  CHECK(WG14_SIGNALS_PREFIX(siguninstall_system)(1) == -1);
  CHECK(errno == EINVAL);

  SECTION("signal_decider_create error paths");
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {0};

  // NULL guarded set.
  errno = 0;
  CHECK(WG14_SIGNALS_PREFIX(signal_decider_create)(
        WG14_SIGNALS_NULLPTR, false, never_called_decider, value) ==
        WG14_SIGNALS_NULLPTR);
  CHECK(errno == EINVAL);

  // Empty guarded set (no signal members).
  sigset_t empty;
  sigemptyset(&empty);
  errno = 0;
  CHECK(WG14_SIGNALS_PREFIX(signal_decider_create)(
        &empty, false, never_called_decider, value) == WG14_SIGNALS_NULLPTR);
  CHECK(errno == EINVAL);

  // Non-empty guarded set but NULL decider.
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGILL);
  errno = 0;
  CHECK(WG14_SIGNALS_PREFIX(signal_decider_create)(
        &guarded, false, WG14_SIGNALS_NULLPTR, value) == WG14_SIGNALS_NULLPTR);
  CHECK(errno == EINVAL);

  return ret;
}
