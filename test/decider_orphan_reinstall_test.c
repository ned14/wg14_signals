#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// Fil-C's runtime reserves SIGILL/SIGTRAP/SIGBUS/SIGSEGV/SIGFPE for its own
// memory-safety mechanism, so the in-tree tests use SIGUSR2 there (like
// stdc_raise_uninstalled_test); SIGABRT is defined and installable everywhere
// else, including Windows/MSVC.
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGABRT
#endif

static enum WG14_SIGNALS_PREFIX(sig_decision)
claiming_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int first_decider_calls = 0;
static int second_decider_calls = 0;
static enum WG14_SIGNALS_PREFIX(sig_decision)
first_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  first_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
}
static enum WG14_SIGNALS_PREFIX(sig_decision)
second_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  second_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

int main(void)
{
  int ret = 0;

  // AA1: siginstall -> decider -> siguninstall (frees the container and orphans
  // the decider node) -> siginstall (new container) -> signal_decider_destroy.
  // Before analysis.md 2.23/AA1 was fixed, destroy LIST_REMOVE'd the orphaned
  // node from the new (empty) list using the node's NULL next/prev and crashed.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *d = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h) == 0);
    void *h2 = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h2 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h2) == 0);
  }

  // Two orphaned nodes: the neighbour pointers must not dangle into the freed
  // container either (heap-use-after-free before the fix).
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *d1 = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    void *d2 = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d1 != WG14_SIGNALS_NULLPTR);
    CHECK(d2 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h) == 0);
    void *h2 = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h2 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d1) == 0);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d2) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h2) == 0);
  }

  // Destroy without reinstall: the orphaned node must still be freed cleanly
  // (no crash). The return is the pre-existing -1 ("not recognised": the signal
  // is not installed), so only the no-crash property is checked here.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *d = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h) == 0);
    (void) WG14_SIGNALS_PREFIX(signal_decider_destroy)(d);
  }

  // Sanity: a normal install -> decider -> destroy -> uninstall cycle must
  // still work.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *d = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, claiming_decider, value);
    CHECK(d != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h) == 0);
  }

  // Sanity for the linked-list link fix this finding relies on: two deciders
  // on the same signal must both be invoked, in addition order, on a raise.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *d1 =
    WG14_SIGNALS_PREFIX(signal_decider_create)(&g, false, first_decider, value);
    void *d2 = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, second_decider, value);
    CHECK(d1 != WG14_SIGNALS_NULLPTR);
    CHECK(d2 != WG14_SIGNALS_NULLPTR);
    first_decider_calls = 0;
    second_decider_calls = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(first_decider_calls == 1);
    CHECK(second_decider_calls == 1);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d1) == 0);
    CHECK(WG14_SIGNALS_PREFIX(signal_decider_destroy)(d2) == 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(h) == 0);
  }

  return ret;
}
