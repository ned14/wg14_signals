#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Regression test for plans/analysis.md 2.4/Z3: on the fallback-TLS path
// sig_global_tss_state_destroy's reset of the static TSS slot was dead code
// after a return, so a full siguninstall left *sig_tss_state_raw() dangling at
// a freed tss_async_signal_safe. Any later re-entry (stdc_raise(0, ...) or
// sigguarded) then thread_init'd the freed handle -> ASan heap-use-after-free.
// The slot must be reset to NULL so the next entry recreates the TSS.

#define INSTALLED_SIGNAL SIGUSR2

static void benign_handler(int signo)
{
  (void) signo;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
reentry_guarded(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  return value;
}
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
reentry_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
reentry_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery);
}

int main(void)
{
  int ret = 0;
  if(SIG_ERR == signal(INSTALLED_SIGNAL, benign_handler))
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
    // Use the library while installed, registering this thread in the TSS on
    // the fallback path.
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
    INSTALLED_SIGNAL, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR));
    // Full uninstall destroys the TSS on the fallback path.
    CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers));
    // Re-entry after a full uninstall must recreate the TSS, not dereference
    // the freed one. stdc_raise(0, ...) performs the non-async-safe setup then
    // returns false.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    // sigguarded() also re-enters via sig_global_tss_state_init().
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGSEGV);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {0};
    (void) WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, reentry_guarded, reentry_recovery, reentry_decider, value);
  }
  printf("post-uninstall re-entry regression checks passed\n");
  return ret;
}
