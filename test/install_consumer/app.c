// Install-consumer smoke test: verifies that a find_package consumer of the
// *installed* wg14_signals package can compile, link and run against the public
// API. It deliberately avoids threads: its job is to catch install / export
// regressions (missing headers, broken Config.cmake, missing version file,
// wrong INTERFACE_* properties), not to test the library's concurrency (that
// is covered by the in-tree tests).
//
// Mirror the feature-test macros the library itself is built with
// (plans/ideas.md 2.2) so the header-only .ipp implementations compiled here
// see the same declarations as the library build. _GNU_SOURCE alone is the
// mirror: on glibc it subsumes the library's _POSIX_C_SOURCE/_XOPEN_SOURCE
// trio, and the BSDs/macOS must NOT get _POSIX_C_SOURCE/_XOPEN_SOURCE at all
// (their <signal.h> hides NSIG — used unguarded by the signo-to-sighandler map
// — under strict POSIX mode, which would break the header-only build).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <wg14_signals/thrd_signal_handle.h>

#include <stdio.h>

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGILL is fine everywhere else
// (SEH-mapped to EXCEPTION_ILLEGAL_INSTRUCTION on Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

static int checks = 0;
static int decider_calls = 0;

#define CHECK(x)                                                               \
  do                                                                           \
  {                                                                            \
    if(!(x))                                                                   \
    {                                                                          \
      fprintf(stderr, "install consumer: CHECK(" #x ") failed at line %d\n",   \
              __LINE__);                                                       \
      return 1;                                                                \
    }                                                                          \
    checks++;                                                                  \
  } while(0)

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  decider_calls++;
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

int main(void)
{
  // The documented one-line per-thread setup call; it returns false for
  // signo == 0 by design (the header documents it as "return immediately
  // doing nothing else").
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);

  // sigfillset_synchronous: must succeed and contain the synchronous-fault
  // signals (SIGILL is in the set on both backends).
  sigset_t synchronous;
  CHECK(WG14_SIGNALS_PREFIX(sigfillset_synchronous)(&synchronous) == 0);
  CHECK(sigismember(&synchronous, SIGILL) == 1);

  // siginstall(NULL): installs handlers for all standard signals and must
  // return a non-NULL handle (which also proves the library linked).
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);

  // A global decider claiming the test signal; stdc_raise must report the
  // claim and the decider must have run exactly once.
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGNAL_TO_USE);
  void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
  &guarded, false, decider_func, value);
  CHECK(decider != WG14_SIGNALS_NULLPTR);
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR));
  CHECK(decider_calls == 1);
  CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider) == 0);

  // sigfence: a compiler-only memory barrier taking the lvalue argument.
  int fence_x = 1;
  sigfence(fence_x);
  CHECK(fence_x == 1);

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  fprintf(stderr, "install consumer: %d checks passed\n", checks);
  return 0;
}
