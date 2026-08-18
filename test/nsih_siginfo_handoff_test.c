#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Regression test for analysis.md NSIH: stdc_raise(signo, NULL, NULL) hands
// off to the previously installed handler. If that handler was installed with
// SA_SIGINFO, it must receive a synthesised minimal siginfo_t (non-NULL, with
// si_signo set) rather than NULL, which would crash any handler that reads
// si->si_signo / si->si_addr without a NULL check. invoke_sigaction is
// POSIX-only, so this test is a no-op on Windows.

#if !defined(_WIN32)

#define SIGNAL_TO_USE SIGUSR2

static volatile sig_atomic_t handler_calls = 0;
static volatile sig_atomic_t saw_valid_si = 0;
static volatile sig_atomic_t saw_signo = 0;
static volatile sig_atomic_t saw_si_code_user = 0;

static void preexisting_siginfo_handler(int signo, siginfo_t *si, void *ctx)
{
  (void) ctx;
  handler_calls++;
  if(si != WG14_SIGNALS_NULLPTR)
  {
    saw_valid_si = 1;
    if(si->si_signo == signo)
    {
      saw_signo = 1;
    }
    if(si->si_code == SI_USER)
    {
      saw_si_code_user = 1;
    }
  }
}

int main(void)
{
  int ret = 0;

  (void) WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);

  // Install a pre-existing SA_SIGINFO handler, then let the library wrap it.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = preexisting_siginfo_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if(0 != sigaction(SIGNAL_TO_USE, &sa, WG14_SIGNALS_NULLPTR))
  {
    fprintf(stderr, "sigaction() failed: %s\n", strerror(errno));
    return 1;
  }

  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGNAL_TO_USE);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    return ret;
  }

  // No deciders claim the raise, so stdc_raise hands off to the pre-existing
  // SA_SIGINFO handler. Before the NSIH fix it received si == NULL and crashed
  // on si->si_signo; it must now receive a synthesised siginfo_t.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR));
  CHECK(handler_calls == 1);
  CHECK(saw_valid_si != 0);
  CHECK(saw_signo != 0);
  CHECK(saw_si_code_user != 0);

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  // siguninstall must restore the pre-existing handler.
  memset(&sa, 0, sizeof(sa));
  CHECK(sigaction(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR, &sa) == 0);
  CHECK((sa.sa_flags & SA_SIGINFO) != 0);

  return ret;
}

#else
int main(void)
{
  // invoke_sigaction is POSIX-only (analysis.md NSIH); nothing to verify on
  // Windows, where the OS info is always present.
  return 0;
}
#endif
