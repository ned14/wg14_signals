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
  void *raw_context;
#ifndef _WIN32
  // Captured by value inside the decider: raw_info points at the default object
  // stored in the per-thread state, which is valid only for the duration of the
  // call, so it must not be dereferenced afterwards.
  int raw_si_signo;
  int raw_si_code;
#endif
};

static struct observed_t observed;

static enum WG14_SIGNALS_PREFIX(sig_decision)
capture_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  observed.decider_calls++;
  observed.signo = rsi->signo;
  observed.error_code = rsi->error_code;
  observed.addr = rsi->addr;
  observed.raw_info = rsi->raw_info;
  observed.raw_context = rsi->raw_context;
#ifndef _WIN32
  if(rsi->raw_info != WG14_SIGNALS_NULLPTR)
  {
    observed.raw_si_signo = ((siginfo_t *) rsi->raw_info)->si_signo;
    observed.raw_si_code = ((siginfo_t *) rsi->raw_info)->si_code;
  }
#endif
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
  // decider call in the same frame must observe the constructed default, not
  // the first raise's raw_info pointer. (N3924 7.14.2.9 p7: a NULL raw_info
  // yields a constructed implementation-defined default, never NULL.)
  siginfo_t fake_info;
  memset(&fake_info, 0, sizeof(fake_info));
  fake_info.si_addr = (void *) (intptr_t) 0x1234;
  (void) WG14_SIGNALS_PREFIX(sighandle)(SIGNAL_TO_USE, &fake_info,
                                        WG14_SIGNALS_NULLPTR);
  (void) WG14_SIGNALS_PREFIX(sighandle)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR);
  return value;
}
#endif

int main(void)
{
  int ret = 0;

  (void) WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR);

  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);

  // Global decider path: sighandle(signo, NULL, NULL) must hand the decider a
  // constructed implementation-defined default (N3924 7.14.2.9 p7/p8), not a
  // NULL raw_info/raw_context. Before the fix the fresh stack `rsi` in
  // sighandle was uninitialised garbage, so error_code and addr were
  // indeterminate and raw_info a garbage pointer.
  {
    memset(&observed, 0, sizeof(observed));
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(sighandle)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR));
    CHECK(observed.decider_calls == 1);
    CHECK(observed.signo == SIGNAL_TO_USE);
    CHECK(observed.error_code == 0);
    CHECK(observed.addr == WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_info != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_context != WG14_SIGNALS_NULLPTR);
#ifndef _WIN32
    CHECK(observed.raw_si_signo == SIGNAL_TO_USE);
    CHECK(observed.raw_si_code == SI_USER);
    // The NULL raw_context was replaced by a context captured from the calling
    // frame, not a zeroed object (7.14.2.9 p8): at least the captured machine
    // state must make it differ from all-zero. The object lives in the
    // per-thread state, so it is still valid here after sighandle() returned.
    {
      static const unsigned char
      zero_context[sizeof(WG14_SIGNALS_PREFIX(stdc_siginfo_context_t))];
      CHECK(memcmp(observed.raw_context, zero_context,
                   sizeof(WG14_SIGNALS_PREFIX(stdc_siginfo_context_t))) != 0);
    }
#endif
    WG14_SIGNALS_PREFIX(signal_decider_destroy(decider));
  }

#ifndef _WIN32
  // Frame path: a second raise with NULL info in the same guarded frame must
  // hand the decider a freshly constructed default, not the first raise's
  // stale raw_info pointer.
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
    CHECK(observed.raw_info != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_context != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_si_signo == SIGNAL_TO_USE);
    CHECK(observed.raw_si_code == SI_USER);
  }
#endif

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
