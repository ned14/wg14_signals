#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Test for the siginstall_set_default_action_np() public API (the replacement
// for the former WG14_SIGNALS_DEFAULT_ACTION compile-time embedder macro): a
// callback installed with the API is invoked with the original siginfo (in
// place of the library's built-in reset-and-re-raise) when stdc_raise() falls
// through to a previously installed SIG_DFL handler, and resetting to NULL
// restores the built-in behaviour. POSIX-only (the Windows backend takes
// default actions via the SEH machinery, so the API returns ENOTSUP there).
static int callback_calls = 0;
static int callback_signo = 0;
static intptr_t callback_info_addr = 0;

static void
default_action_cb(int signo, WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * info,
                  void *context)
{
  (void) context;
  callback_calls++;
  callback_signo = signo;
  callback_info_addr = (intptr_t) info;
}

int main(void)
{
  int ret = 0;
#ifdef _WIN32
  // The API is POSIX-only: the Windows backend takes default actions via the
  // SEH machinery, so the call must fail with ENOTSUP.
  errno = 0;
  CHECK(-1 == WG14_SIGNALS_PREFIX(siginstall_set_default_action_np)(
              WG14_SIGNALS_NULLPTR));
  CHECK(ENOTSUP == errno);
  return ret;
#else
  // Phase 1: install a custom default-action callback. SIGUSR2's default
  // action terminates the process, so a raise falling through to it exercises
  // the callback path (which returns without re-raising) versus the built-in
  // path (which would kill the test).
  SECTION("custom callback");
  errno = 0;
  CHECK(0 == WG14_SIGNALS_PREFIX(siginstall_set_default_action_np)(
             default_action_cb));

  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGUSR2);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = SIGUSR2;
  info.si_code = SI_USER;
  // No decider claims the raise, so stdc_raise() falls through to the
  // previously installed (SIG_DFL) handler and the default-action callback
  // runs with the original siginfo.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGUSR2, &info, WG14_SIGNALS_NULLPTR));
  CHECK(1 == callback_calls);
  CHECK(SIGUSR2 == callback_signo);
  CHECK((intptr_t) &info == callback_info_addr);
  // The callback ran instead of a reset-and-re-raise, so the library's
  // handler is still installed and the process survived SIGUSR2.
  struct sigaction installed;
  memset(&installed, 0, sizeof(installed));
  CHECK(0 == sigaction(SIGUSR2, WG14_SIGNALS_NULLPTR, &installed));
  CHECK(installed.sa_sigaction != WG14_SIGNALS_NULLPTR);
  CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers));

  // Phase 2: resetting to NULL restores the library's built-in default
  // action. SIGCONT's default (continue if stopped, else ignore) never
  // terminates the process, and the built-in reset-and-re-raise must preserve
  // the library's installed handler (analysis DFLT).
  SECTION("reset to builtin default");
  errno = 0;
  CHECK(0 == WG14_SIGNALS_PREFIX(siginstall_set_default_action_np)(
             WG14_SIGNALS_NULLPTR));
  sigset_t guarded2;
  sigemptyset(&guarded2);
  sigaddset(&guarded2, SIGCONT);
  void *handlers2 = WG14_SIGNALS_PREFIX(siginstall)(&guarded2);
  CHECK(handlers2 != WG14_SIGNALS_NULLPTR);
  if(handlers2 == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGCONT, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR));
  // The custom callback must not have been invoked again.
  CHECK(1 == callback_calls);
  struct sigaction after;
  memset(&after, 0, sizeof(after));
  CHECK(0 == sigaction(SIGCONT, WG14_SIGNALS_NULLPTR, &after));
  CHECK(after.sa_sigaction != WG14_SIGNALS_NULLPTR);
  CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers2));
  return ret;
#endif
}
