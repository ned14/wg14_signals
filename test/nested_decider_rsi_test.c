#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

// Regression test for analysis NSTR: with SA_NODEFER a second delivery of a
// guarded signal during a decider call re-enters stdc_raise on the same frame.
// Each raise must get its own rsi; a shared `frame->rsi` would be overwritten
// mid-decider by the nested raise's prepare_rsi, so the outer decider would
// read the nested raise's raw_info/signo (torn fields). Fil-C's runtime
// reserves SIGILL/SIGSEGV/SIGFPE for its own memory-safety mechanism, so the
// tests use SIGUSR2 there; SIGILL is defined and installable everywhere else.
// The bug is POSIX-frame-specific (on Windows the frame rsi is a fresh local in
// win32_exception_filter), so the body is POSIX-only.
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

static atomic_int outer_decider_calls = 0;
static atomic_int nested_decider_calls = 0;
// 0 = outer rsi clean after the nested delivery; 1 = outer rsi clobbered by the
// nested raise (the NSTR defect); 2 = outer rsi already non-NULL at entry.
static volatile int outer_saw_nested_info = 0;
// Set if the nested decider itself saw an unexpected raw_info/signo.
static volatile int nested_saw_bad_info = 0;

// Both the outer and the nested delivery run this decider. The outer call
// triggers a real nested delivery of the same signal via pthread_kill and, once
// it has returned, verifies its own rsi was not clobbered (the outer raise
// passed NULL info, so raw_info must still be NULL; the nested delivery carries
// the kernel's real siginfo, which is what the old shared-frame-rsi bug would
// leave in the outer decider's view).
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
nested_rsi_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  if(0 == atomic_fetch_add(&outer_decider_calls, 1))
  {
    // First (outer) activation: software raise with NULL info.
    if(rsi->raw_info != WG14_SIGNALS_NULLPTR)
    {
      outer_saw_nested_info = 2;
    }
    // Re-enter the same frame's decider via a real signal delivery. SA_NODEFER
    // leaves the signal unblocked inside the handler, so the nested delivery
    // interrupts this call; the kernel siginfo is what the nested raise's
    // prepare_rsi used to write into the shared frame->rsi.
    pthread_kill(pthread_self(), SIGNAL_TO_USE);
    if(rsi->raw_info != WG14_SIGNALS_NULLPTR)
    {
      outer_saw_nested_info = 1;
    }
    return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
  }
  // Nested activation: real signal delivery, kernel siginfo must be present.
  atomic_fetch_add(&nested_decider_calls, 1);
  if(rsi->raw_info == WG14_SIGNALS_NULLPTR || rsi->signo != SIGNAL_TO_USE)
  {
    nested_saw_bad_info = 1;
  }
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
raise_inside_guard(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // Software raise with NULL info: the outer decider must see raw_info == NULL.
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

int main(void)
{
  int ret = 0;

#ifndef _WIN32
  SECTION(
  "Nested delivery during a frame decider does not corrupt the outer rsi");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers != WG14_SIGNALS_NULLPTR);
    atomic_store(&outer_decider_calls, 0);
    atomic_store(&nested_decider_calls, 0);
    outer_saw_nested_info = 0;
    nested_saw_bad_info = 0;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret_value =
    WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, raise_inside_guard, noop_recovery, nested_rsi_decider,
    (union WG14_SIGNALS_PREFIX(stdc_siginfo_value)) {.int_value = 7});
    // Both activations ran: the outer software raise and the nested real
    // delivery (the counter is incremented by both).
    CHECK(atomic_load(&outer_decider_calls) == 2);
    CHECK(atomic_load(&nested_decider_calls) == 1);
    // The nested decider saw the kernel siginfo of its own delivery.
    CHECK(!nested_saw_bad_info);
    // The outer decider's rsi survived the nested delivery unclobbered.
    CHECK(outer_saw_nested_info == 0);
    CHECK(ret_value.int_value == 7);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
    // After uninstall the frame is popped and the map entry gone: an unclaimed
    // raise returns false instead of re-raising SIGILL at the default action.
    CHECK(!WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                           WG14_SIGNALS_NULLPTR));
    CHECK(atomic_load(&outer_decider_calls) == 2);
  }
#endif

  return ret;
}
