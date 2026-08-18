#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Regression test for plans/analysis.md DFLT: when stdc_raise() falls through
// to a SIG_DFL previously-installed handler for a signal whose default action
// does not terminate the process, invoke_sigaction in the POSIX backend used to
// reset the kernel handler to SIG_DFL and re-raise, permanently discarding the
// library's installed handler. After the process resumes from a stop, later
// deliveries of that signal would bypass the library until a re-install.
// invoke_sigaction is POSIX-only, so this test is a no-op on Windows.
// SIGCONT is used because its default action ("continue if stopped, else
// ignore") never blocks a running test process; the fix's restore path is the
// same for the stop signals (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU).
int main(void)
{
  int ret = 0;
#ifdef _WIN32
  // invoke_sigaction is POSIX-only (analysis DFLT).
  return ret;
#else
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGCONT);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }
  struct sigaction before, after;
  memset(&before, 0, sizeof(before));
  CHECK(0 == sigaction(SIGCONT, WG14_SIGNALS_NULLPTR, &before));
  // The library's handler must be installed for SIGCONT before the raise.
  CHECK(before.sa_handler != SIG_DFL);
  CHECK(before.sa_handler != SIG_IGN);
  // No decider claims the raise, so stdc_raise() falls through to the
  // previously installed (SIG_DFL) handler. Taking that default action must
  // not permanently discard the library's installed handler (analysis DFLT).
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGCONT, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR));
  memset(&after, 0, sizeof(after));
  CHECK(0 == sigaction(SIGCONT, WG14_SIGNALS_NULLPTR, &after));
  CHECK(after.sa_handler != SIG_DFL);
  CHECK(after.sa_handler != SIG_IGN);
  // The very same handler object survived the default-action re-raise.
  CHECK(after.sa_handler == before.sa_handler);
  CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers));
  printf("SIG_DFL default-action re-raise preserves the installed handler\n");
  return ret;
#endif
}
