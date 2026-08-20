// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): the
// glibc-internal signal names (SIGCANCEL/SIGSETXID) and the SIGRTMIN/SIGRTMAX
// macros are only exposed under __USE_GNU on glibc, so define _GNU_SOURCE as
// the library's FTM probe does. The BSDs/macOS must not get the _POSIX_C_SOURCE
// trio (it hides NSIG there).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

// Regression test for analysis.md RTIM: siginstall(NULL) installs handlers for
// "all the standard POSIX signals", which must NOT include libc-internal
// signals (SIGCANCEL/SIGSETXID on glibc) or the realtime range. Pre-fix the
// returned handle was a raw sigfillset(), so on glibc it covered 32/33 and all
// realtime signals; now those are filtered out while the standard signals
// remain.

int main(void)
{
  int ret = 0;
  sigset_t *set = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(set != WG14_SIGNALS_NULLPTR);
  if(set == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }

  // The standard signals must all be covered.
  CHECK(sigismember(set, SIGILL) == 1);
  CHECK(sigismember(set, SIGTERM) == 1);
  CHECK(sigismember(set, SIGSEGV) == 1);
  CHECK(sigismember(set, SIGUSR1) == 1);

  // libc-internal signals must be excluded (glibc only; other libcs do not
  // define these names).
#ifdef SIGCANCEL
  CHECK(sigismember(set, SIGCANCEL) != 1);
#endif
#ifdef SIGSETXID
  CHECK(sigismember(set, SIGSETXID) != 1);
#endif

  // The realtime range must be excluded. SIGRTMIN/SIGRTMAX are compile-time
  // constants on musl/BSD and runtime functions on glibc; both evaluate fine
  // here (this is not a signal handler).
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  for(int signo = (int) SIGRTMIN; signo <= (int) SIGRTMAX; signo++)
  {
    CHECK(sigismember(set, signo) != 1);
  }
#endif

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(set) == 0);

  // The same libc-internal filtering must apply to explicit guarded inputs: a
  // caller doing sigfillset() then siginstall(&set) must not get handlers
  // installed for SIGCANCEL/SIGSETXID either (analysis.md RTIM). Realtime
  // signals, by contrast, are a legitimate explicit user choice and are kept.
  SECTION("explicit guarded input is also filtered");
  sigset_t guarded;
  sigfillset(&guarded);
  sigset_t *explicit = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(explicit != WG14_SIGNALS_NULLPTR);
  if(explicit == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }
#ifdef SIGCANCEL
  CHECK(sigismember(explicit, SIGCANCEL) != 1);
#endif
#ifdef SIGSETXID
  CHECK(sigismember(explicit, SIGSETXID) != 1);
#endif
  // Standard signals remain installable via the explicit path.
  CHECK(sigismember(explicit, SIGILL) == 1);
  CHECK(sigismember(explicit, SIGTERM) == 1);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(explicit) == 0);
  return ret;
}
