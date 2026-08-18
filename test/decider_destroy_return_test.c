#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests use SIGUSR2 there; SIGABRT is fine everywhere else
// (SEH-mapped to EXCEPTION_NONCONTINUABLE_EXCEPTION on Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGABRT
#endif

// Deliberately never installed in this test, so it exercises the warning-path
// NULL slot in signal_decider_create(). SIGUSR1 would be the natural choice but
// is not defined on MSVC, so use SIGTERM (defined on every target and distinct
// from both SIGNAL_TO_USE values).
#define OTHER_SIGNAL SIGTERM

static enum WG14_SIGNALS_PREFIX(sig_decision)
claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

int main(void)
{
  int ret = 0;
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};

  // SDCF: destroying a decider handle whose slots are all NULL (every guarded
  // signal had no handler installed at create time -- the warning path) must
  // report success, 0 per the N3924 7.14.2.8 return contract ("If successful,
  // this function returns zero"). Pre-fix it returned -1 with errno unchanged
  // because no slot matched, even though the handle was removed
  // (plans/analysis.md SDCF).
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    void *d = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d != WG14_SIGNALS_NULLPTR);
    errno = 0;
    CHECK(0 == WG14_SIGNALS_PREFIX(signal_decider_destroy)(d));
    CHECK(errno == 0);
  }

  // A mixed handle -- one installed signal carrying a live decider node and one
  // warning-path NULL slot -- is the shape of the partially built handle that
  // signal_decider_create()'s failure path destroys. It must destroy cleanly
  // with 0 as well, and the installed node must be unlinked so the container
  // survives a later siguninstall.
  {
    sigset_t install;
    sigemptyset(&install);
    sigaddset(&install, SIGNAL_TO_USE);
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&install);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    sigaddset(&g, OTHER_SIGNAL);
    void *d = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d != WG14_SIGNALS_NULLPTR);
    CHECK(0 == WG14_SIGNALS_PREFIX(signal_decider_destroy)(d));
    CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(h));
  }

  // Contract sanity: a NULL handle is still a failure (EINVAL, -1), which is
  // the only genuinely unsuccessful destroy.
  errno = 0;
  CHECK(-1 ==
        WG14_SIGNALS_PREFIX(signal_decider_destroy)(WG14_SIGNALS_NULLPTR));
  CHECK(EINVAL == errno);

  printf("decider-destroy return contract checks passed\n");
  return ret;
}
