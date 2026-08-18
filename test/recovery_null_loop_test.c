/* NOT RUN under Fil-C (excluded from the Fil-C ctest run, ci.yml): this test
   recovers from a genuine SIGSEGV raised by a store through a null pointer.
   Fil-C's memory-safety pass intercepts the null-capability access itself
   ("cannot write pointer with null object" panic) before any fault signal is
   delivered, so the library's SIGSEGV handler is never invoked and recovery is
   impossible on Fil-C -- a read would trap just the same (analysis.md 5.10). */
#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A store to this address faults on every supported platform (Linux, macOS,
   Windows). Volatile and opaque so the compiler cannot fold it into a null
   pointer constant and warn, or elide the store. */
static volatile uintptr_t bad_address = 0;

#ifndef SIGSEGV
#define SIGSEGV 11
#endif

struct shared_t
{
  volatile int inner_decider_calls;
  int outer_decider_calls;
  int outer_recovery_calls;
  int recovered_signo;
};

/* The inner decider asks to invoke recovery, but the inner sigguarded has no
   recovery function. Before the analysis.md 1.7 fix both backends sent the
   exception back to the faulting instruction, which re-faulted forever
   (POSIX: stdc_raise returns true; Windows: EXCEPTION_CONTINUE_EXECUTION).
   Detect that infinite loop and terminate the test fast instead of hanging. */
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
inner_decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  if(++shared->inner_decider_calls > 100)
  {
    _Exit(2); /* 1.7 infinite fault loop detected */
  }
  return WG14_SIGNALS_PREFIX(sig_decision_call_recovery);
}

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
outer_decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  shared->outer_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_call_recovery);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
outer_recovery_func(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  shared->outer_recovery_calls++;
  shared->recovered_signo = rsi->signo;
  return rsi->value;
}

/* Trigger a genuine synchronous fault. The kernel re-executes this instruction
   whenever the library handler returns without recovering, which is exactly
   the 1.7 infinite fault loop. */
static
#ifdef __clang__
__attribute__((no_sanitize("address", "undefined")))
#elif defined(__GNUC__)
__attribute__((no_sanitize_undefined))
#endif
union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
fault_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  (void) value;
  *(volatile int *) bad_address = 1;
  return value;
}

/* The inner sigguarded guards SIGSEGV but supplies recovery == NULL, the 1.7
   trigger. */
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
call_inner(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGSEGV);
  return WG14_SIGNALS_PREFIX(sigguarded)(
  &guarded, fault_func, WG14_SIGNALS_NULLPTR, inner_decider_func, value);
}

int main(void)
{
  int ret = 0;
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "FATAL: siginstall() failed with %s\n", strerror(errno));
    return 1;
  }

  struct shared_t shared = {0};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.ptr_value = &shared};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGSEGV);
  /* When the inner NULL-recovery frame declines, the exception must fall
     through to this outer frame, whose recovery runs. */
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, call_inner, outer_recovery_func, outer_decider_func, value);

  CHECK(shared.outer_recovery_calls == 1);
  CHECK(shared.recovered_signo == SIGSEGV);
  CHECK(shared.outer_decider_calls == 1);
  CHECK(shared.inner_decider_calls == 1);

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  return ret;
}
