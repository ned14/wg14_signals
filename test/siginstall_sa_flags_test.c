#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Test for the siginstall_set_sa_flags_np() public API (the replacement for
// the former WG14_SIGNALS_SA_FLAGS compile-time embedder macro): the sa_flags
// the raw signal handler is installed with must be exactly what the API
// requested, the default (no call) must be SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER,
// a flag set without SA_SIGINFO must be rejected with EINVAL without changing
// the stored flags, and the setting must apply to every siginstall() performed
// after the call. The raw handler is installed via sigaction only on POSIX,
// so this test is a no-op on Windows (where the API returns ENOTSUP).
//
// SA_NOCLDWAIT is passed through to sigaction() but is not reported back on
// every platform (macOS only reports it for SIGCHLD), so the checks below
// cover SA_SIGINFO and SA_NODEFER -- the flags every platform round-trips --
// while the caller still requests the full default including SA_NOCLDWAIT.
static int check_installed_flags(const int expected_present,
                                 const int expected_absent)
{
  int ret = 0;
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
  struct sigaction installed;
  memset(&installed, 0, sizeof(installed));
  CHECK(0 == sigaction(SIGUSR2, WG14_SIGNALS_NULLPTR, &installed));
  CHECK(installed.sa_sigaction != WG14_SIGNALS_NULLPTR);
  const int portably_reported = SA_SIGINFO | SA_NODEFER;
  CHECK((installed.sa_flags & expected_present & portably_reported) ==
        (expected_present & portably_reported));
  CHECK((installed.sa_flags & expected_absent & portably_reported) == 0);
  CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers));
  return ret;
}

int main(void)
{
  int ret = 0;
#ifdef _WIN32
  // The API is POSIX-only: on Windows the raw handler is not installed via
  // sigaction, so the call must fail with ENOTSUP.
  errno = 0;
  CHECK(-1 == WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(SA_SIGINFO));
  CHECK(ENOTSUP == errno);
  return ret;
#else
  // Phase 1: no setter call -- the raw handler is installed with the library
  // default SA_SIGINFO|SA_NOCLDWAIT|SA_NODEFER.
  SECTION("default flags");
  ret += check_installed_flags(SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER, 0);

  // Phase 2: a minimal flag set (SA_SIGINFO only) is honoured exactly, and
  // the default's extra flags are not present.
  SECTION("minimal flags");
  errno = 0;
  CHECK(0 == WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(SA_SIGINFO));
  ret += check_installed_flags(SA_SIGINFO, SA_NOCLDWAIT | SA_NODEFER);

  // Phase 3: a flag set without SA_SIGINFO is rejected with EINVAL and must
  // not change the stored flags (the next install still uses SA_SIGINFO only).
  SECTION("reject without SA_SIGINFO");
  errno = 0;
  CHECK(-1 == WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(0));
  CHECK(EINVAL == errno);
  errno = 0;
  CHECK(-1 == WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(SA_NODEFER |
                                                              SA_NOCLDWAIT));
  CHECK(EINVAL == errno);
  ret += check_installed_flags(SA_SIGINFO, SA_NOCLDWAIT | SA_NODEFER);

  // Phase 4: restoring the default flag set applies to the next install.
  SECTION("restore default flags");
  errno = 0;
  CHECK(0 == WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(
             SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER));
  ret += check_installed_flags(SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER, 0);
  return ret;
#endif
}
