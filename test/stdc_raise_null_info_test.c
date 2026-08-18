#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <string.h>

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGILL is fine everywhere else
// (SEH-mapped to EXCEPTION_ILLEGAL_INSTRUCTION on Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

struct observed_t
{
  int decider_calls;
  int signo;
  WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t) error_code;
  void *addr;
  void *raw_info;
};

static struct observed_t observed;

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
capture_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  observed.decider_calls++;
  observed.signo = rsi->signo;
  observed.error_code = rsi->error_code;
  observed.addr = rsi->addr;
  observed.raw_info = rsi->raw_info;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

#ifndef _WIN32
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // First raise supplies a real siginfo, second a NULL info: the second
  // decider call in the same frame must observe the zeroed fields, not the
  // first raise's raw_info pointer (analysis.md 2.14/W2).
  siginfo_t fake_info;
  memset(&fake_info, 0, sizeof(fake_info));
  fake_info.si_addr = (void *) (intptr_t) 0x1234;
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, &fake_info,
                                         WG14_SIGNALS_NULLPTR);
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}
#endif

int main(void)
{
  int ret = 0;

  (void) WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);

  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);

  // Global decider path: stdc_raise(signo, NULL, NULL) must hand the decider
  // deterministic zero/NULL fields (analysis.md 2.14/W2). Before the fix the
  // fresh stack `rsi` in stdc_raise was uninitialised garbage, so error_code
  // and addr were indeterminate and raw_info a garbage pointer.
  {
    memset(&observed, 0, sizeof(observed));
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR));
    CHECK(observed.decider_calls == 1);
    CHECK(observed.signo == SIGNAL_TO_USE);
    CHECK(observed.error_code == 0);
    CHECK(observed.addr == WG14_SIGNALS_NULLPTR);
#ifdef _WIN32
    // Windows always has OS info: raw_info points at the EXCEPTION_RECORD.
    CHECK(observed.raw_info != WG14_SIGNALS_NULLPTR);
#else
    CHECK(observed.raw_info == WG14_SIGNALS_NULLPTR);
#endif
    WG14_SIGNALS_PREFIX(signal_decider_destroy(decider));
  }

#ifndef _WIN32
  // Frame path: a second raise with NULL info in the same guarded frame must
  // not hand the decider the first raise's stale raw_info pointer.
  {
    memset(&observed, 0, sizeof(observed));
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    (void) WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, guarded_func, noop_recovery, capture_decider, value);
    CHECK(observed.decider_calls == 2);
    CHECK(observed.signo == SIGNAL_TO_USE);
    CHECK(observed.error_code == 0);
    CHECK(observed.addr == WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_info == WG14_SIGNALS_NULLPTR);
  }
#endif

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
