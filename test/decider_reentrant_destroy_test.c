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

// Reentrant signal_decider_destroy() contract (N3924 7.14.2.8; plans
// proposed-wording-fixes.md 7.3/17): calling it from within a decider function
// is permitted. The node refcount protocol (registry ref = 1, in-flight raise
// +1 during the unlocked decider call) defers the free of a currently
// executing decider's node until the raise completes, and a sibling node is
// freed with the in-flight walk relinked past it by LIST_REMOVE.

static void *self_handle = WG14_SIGNALS_NULLPTR;
static int self_destroy_calls = 0;
static int self_destroy_result = -99;

static enum WG14_SIGNALS_PREFIX(sig_decision)
self_destroying_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  self_destroy_calls++;
  errno = 0;
  self_destroy_result =
  WG14_SIGNALS_PREFIX(signal_decider_destroy)(self_handle);
  // The handle is invalid after destroy; never destroy it again.
  self_handle = WG14_SIGNALS_NULLPTR;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static int survivor_calls = 0;

static enum WG14_SIGNALS_PREFIX(sig_decision)
survivor_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  survivor_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static void *sibling_handle = WG14_SIGNALS_NULLPTR;
static int sibling_a_calls = 0;
static int sibling_b_calls = 0;
static int sibling_destroy_result = -99;

static enum WG14_SIGNALS_PREFIX(sig_decision)
sibling_a_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  sibling_a_calls++;
  // Destroy B's handle on the first invocation only: the handle is invalid
  // after destroy, and calling destroy again on later raises would be
  // undefined behaviour (double destroy).
  if(sibling_handle != WG14_SIGNALS_NULLPTR)
  {
    errno = 0;
    sibling_destroy_result =
    WG14_SIGNALS_PREFIX(signal_decider_destroy)(sibling_handle);
    sibling_handle = WG14_SIGNALS_NULLPTR;
  }
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static enum WG14_SIGNALS_PREFIX(sig_decision)
sibling_b_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  sibling_b_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

int main(void)
{
  int ret = 0;
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};

  // Self-destroy: the executing decider destroys its own handle. The node is
  // not freed while the decider is still executing (the free is deferred until
  // the raise completes and the node is retired via deferred_frees), the
  // destroy reports 0, and a later raise no longer invokes the decider. A
  // second handle on the same signal must survive the whole sequence.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    self_handle = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, self_destroying_decider, value);
    CHECK(self_handle != WG14_SIGNALS_NULLPTR);
    void *survivor = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, survivor_decider, value);
    CHECK(survivor != WG14_SIGNALS_NULLPTR);
    self_destroy_calls = 0;
    self_destroy_result = -99;
    survivor_calls = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(self_destroy_calls == 1);
    CHECK(self_destroy_result == 0);
    CHECK(errno == 0);
    // The raise was claimed by the self-destroying decider, so the survivor
    // was not consulted in this raise.
    CHECK(survivor_calls == 0);
    // The destroyed decider's node was retired when the first raise completed:
    // a second raise must not invoke it, and the surviving handle still works.
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(self_destroy_calls == 1);
    CHECK(survivor_calls == 1);
    CHECK(0 == WG14_SIGNALS_PREFIX(signal_decider_destroy)(survivor));
    CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(h));
    self_handle = WG14_SIGNALS_NULLPTR;
  }

  // Sibling-destroy: a callfirst decider destroys a later decider's handle
  // from within its own invocation. The mid-walk LIST_REMOVE relinks the
  // in-flight walk so the destroyed decider is invoked by neither the same
  // raise nor any later raise.
  {
    sigset_t g;
    sigemptyset(&g);
    sigaddset(&g, SIGNAL_TO_USE);
    void *h = WG14_SIGNALS_PREFIX(siginstall)(&g);
    CHECK(h != WG14_SIGNALS_NULLPTR);
    void *a = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, true, sibling_a_decider, value);
    CHECK(a != WG14_SIGNALS_NULLPTR);
    sibling_handle = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &g, false, sibling_b_decider, value);
    CHECK(sibling_handle != WG14_SIGNALS_NULLPTR);
    sibling_a_calls = 0;
    sibling_b_calls = 0;
    sibling_destroy_result = -99;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(sibling_a_calls == 1);
    CHECK(sibling_b_calls == 0);
    CHECK(sibling_destroy_result == 0);
    CHECK(errno == 0);
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(sibling_a_calls == 2);
    CHECK(sibling_b_calls == 0);
    CHECK(0 == WG14_SIGNALS_PREFIX(signal_decider_destroy)(a));
    CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(h));
    sibling_handle = WG14_SIGNALS_NULLPTR;
  }

  printf("decider-reentrant-destroy checks passed\n");
  return ret;
}
