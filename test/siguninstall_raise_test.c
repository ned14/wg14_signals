#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>

// Regression test for plans/analysis.md 2.2/W4: while a thread is unlocked
// inside a global decider's stdc_raise, another thread runs
// signal_decider_destroy() (bringing the decider node's refcount to 1) and then
// siguninstall() (which in the unfixed code frees the sighandler_info container
// outright). When the decider returns without claiming the signal, the raise
// re-acquires the lock, its node refcount reaches zero and it re-accesses the
// freed container's global_handler/deferred_frees list heads -> use-after-free.
// The raise holds a reference on the container across the unlocked decider call
// so the container survives until the raise releases it.

#if defined(_WIN32)
// The concurrent raise-vs-uninstall scenario is exercised on POSIX here; the
// Windows vectored-handler path (analysis.md W4/2.15) uses the identical
// refcount protocol but cannot be validated on this host.
int main(void)
{
  return 0;
}
#else

#define INSTALLED_SIGNAL SIGUSR2

static volatile sig_atomic_t old_handler_calls = 0;
static void old_signal_handler(int signo)
{
  (void) signo;
  old_handler_calls++;
}

static atomic_int decider_entered;
static atomic_int uninstall_done;
static atomic_int raise_result;

static enum WG14_SIGNALS_PREFIX(sig_decision)
decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  // The raise is now unlocked inside the decider. Hold until the other thread
  // has run signal_decider_destroy() and siguninstall(), which in the unfixed
  // code free the decider node and the container while we are still inside this
  // call. Returning "next decider" makes the raise's node refcount reach zero
  // after re-locking, which is what re-accesses the container.
  atomic_store_explicit(&decider_entered, 1, memory_order_release);
  while(atomic_load_explicit(&uninstall_done, memory_order_acquire) == 0)
  {
  }
  return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
}

static int raiser_thread(void *unused)
{
  (void) unused;
  atomic_store_explicit(
  &raise_result,
  (int) WG14_SIGNALS_PREFIX(stdc_raise)(INSTALLED_SIGNAL, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR),
  memory_order_release);
  return 0;
}

int main(void)
{
  int ret = 0;
  // Give the installed signal a benign old handler so the raise's fall-through
  // to the previously installed handler is a no-op instead of terminating the
  // process.
  if(SIG_ERR == signal(INSTALLED_SIGNAL, old_signal_handler))
  {
    fprintf(stderr, "signal() failed: %s\n", strerror(errno));
    return 1;
  }

  for(int cycle = 0; cycle < 100; cycle++)
  {
    atomic_store_explicit(&decider_entered, 0, memory_order_relaxed);
    atomic_store_explicit(&uninstall_done, 0, memory_order_relaxed);
    atomic_store_explicit(&raise_result, 0, memory_order_relaxed);

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
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {0};
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &installed, false, decider_func, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    if(decider == WG14_SIGNALS_NULLPTR)
    {
      WG14_SIGNALS_PREFIX(siguninstall)(handlers);
      return ret;
    }

    thrd_t thr;
    int res = 0;
    // Compare against thrd_success: FreeBSD's <threads.h> defines thrd_success
    // as 4 (not 0), so "0 != thrd_create(...)" would misreport success here.
    if(thrd_success != thrd_create(&thr, raiser_thread, WG14_SIGNALS_NULLPTR))
    {
      CHECK(0);
      (void) WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
      WG14_SIGNALS_PREFIX(siguninstall)(handlers);
      return ret;
    }
    // Wait until the raiser is inside the decider (unlocked), then retire the
    // decider node and uninstall the handler. Without the fix the container is
    // freed here while the raise is still inside the decider call.
    while(atomic_load_explicit(&decider_entered, memory_order_acquire) == 0)
    {
    }
    CHECK(0 == WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider));
    const int unr = WG14_SIGNALS_PREFIX(siguninstall)(handlers);
    atomic_store_explicit(&uninstall_done, 1, memory_order_release);
    CHECK(0 == unr);
    thrd_join(thr, &res);
    CHECK(res == 0);
    // The raise must have completed without touching freed memory.
    CHECK(atomic_load_explicit(&raise_result, memory_order_acquire) == 1);
  }
  printf("siguninstall/signal_decider_destroy vs in-flight raise regression "
         "checks passed\n");
  return ret;
}
#endif
