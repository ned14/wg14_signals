/* NOT RUN under Fil-C (excluded from the Fil-C ctest run, ci.yml): this test
   recovers from a genuine SIGFPE raised by an integer divide-by-zero. Fil-C's
   runtime reserves SIGILL/SIGTRAP/SIGBUS/SIGSEGV/SIGFPE for its own
   memory-safety mechanism: sigaction() on them returns ENOSYS, so the library
   cannot install a handler for SIGFPE and the fault kills the process
   (analysis.md 5.10). */
#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>

// ThreadSanitizer proxies every user-installed signal handler through its own
// sighandler. When the library's sigguarded() recovery longjmps out of the
// handler, TSan's per-thread signal state (in_signal_handler counter, signal
// mask) is never restored, so on x86_64 a subsequent hardware SIGFPE can be
// mishandled and terminate the process with an unhandled SIGFPE (analysis.md
// 5.4 TSan finding). Under TSan, skip the hardware divide-by-zero and drive
// SIGFPE through stdc_raise() instead -- the same decider/recovery path, but
// with no kernel-delivered fault (the no-trap architectures already do this).
// The hardware-fault path stays covered by the ASan/UBSan and unsanitised CI
// legs.
#if defined(__SANITIZE_THREAD__)
#define WG14_SIGFPE_TEST_TSAN 1
#endif
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WG14_SIGFPE_TEST_TSAN 1
#endif
#endif

/* Use SIGFPE for all tests */
#ifndef SIGFPE
#define SIGFPE 8
#endif

extern volatile int divisor;
volatile int divisor;

struct shared_t
{
  int count_decider, count_recovery;
  int test_value;
};

/* Recovery function for SIGFPE tests */
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
sigfpe_recovery_func(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  shared->count_recovery++;
  /* Store the signal number for verification */
  shared->test_value = rsi->signo;
  return rsi->value;
}

/* Decider function for SIGFPE tests */
static enum WG14_SIGNALS_PREFIX(sig_decision_t)
sigfpe_decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  shared->count_decider++;
  /* Verify we got SIGFPE */
  if(rsi->signo == SIGFPE)
  {
    shared->test_value = SIGFPE;
  }
  return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery); /* handled */
}

/* Guarded function that triggers SIGFPE via division by zero */
static
#ifdef __clang__
__attribute__((no_sanitize("undefined")))
#elif defined(__GNUC__)
__attribute__((no_sanitize_undefined))
#endif
union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
sigfpe_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
#ifdef WG14_SIGFPE_TEST_TSAN
  /* TSan cannot recover a hardware SIGFPE (see the TSan probe above), so use a
     non-faulting divide and let the stdc_raise() below drive the
     decider/recovery path. */
  volatile int result = 42 / 1;
#else
  /* This should trigger SIGFPE */
  volatile int result = 42 / divisor;
#endif
  /* Not necessary, only here so we're unit testing sigfence() */
  sigfence(result);
  /* If we get here, this architecture doesn't trap integer divide by zero */
  WG14_SIGNALS_PREFIX(stdc_raise)(SIGFPE, WG14_SIGNALS_NULLPTR,
                                  WG14_SIGNALS_NULLPTR);
  (void) result;
  return value;
}

/* Test 1: Thread-local SIGFPE handling */
static int test_thread_local_sigfpe(void)
{
  int ret = 0;
  puts("Test 1: Thread-local SIGFPE handling ...");
  struct shared_t shared = {
  .count_decider = 0, .count_recovery = 0, .test_value = 0};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.ptr_value = &shared};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGFPE);
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, sigfpe_func, sigfpe_recovery_func, sigfpe_decider_func, value);
  CHECK(shared.count_decider == 1);
  CHECK(shared.count_recovery == 1);
  CHECK(shared.test_value == SIGFPE);
  return ret;
}

/* Test 2: Global SIGFPE handling */
static int test_global_sigfpe(void)
{
  int ret = 0;
  puts("Test 2: Global SIGFPE handling ...");
  struct shared_t shared = {
  .count_decider = 0, .count_recovery = 0, .test_value = 0};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.ptr_value = &shared};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGFPE);
  void *sigfpe_decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
  &guarded, false, sigfpe_decider_func, value);
  /* Trigger SIGFPE via stdc_raise */
  WG14_SIGNALS_PREFIX(stdc_raise)
  (SIGFPE, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  CHECK(shared.count_decider == 1);
  /* Recovery is not called for global handlers */
  CHECK(shared.count_recovery == 0);
  CHECK(shared.test_value == SIGFPE);
  WG14_SIGNALS_PREFIX(signal_decider_destroy(sigfpe_decider));
  return ret;
}

/* Test 3: SIGFPE with custom value */
static int test_sigfpe_with_custom_value(void)
{
  int ret = 0;
  puts("Test 3: SIGFPE with custom value ...");
  struct shared_t shared = {
  .count_decider = 0, .count_recovery = 0, .test_value = 0};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.ptr_value = &shared};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGFPE);
  /* Pass a custom value through sigguarded */
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, sigfpe_func, sigfpe_recovery_func, sigfpe_decider_func, value);
  CHECK(shared.count_decider == 1);
  CHECK(shared.count_recovery == 1);
  /* Verify the value was accessible in decider and recovery */
  CHECK(shared.test_value == SIGFPE);
  return ret;
}

/* Test 4: Multiple SIGFPE invocations */
static int test_multiple_sigfpe_invocations(void)
{
  int ret = 0;
  puts("Test 4: Multiple SIGFPE invocations ...");
  struct shared_t shared = {
  .count_decider = 0, .count_recovery = 0, .test_value = 0};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.ptr_value = &shared};
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGFPE);

  /* First invocation */
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, sigfpe_func, sigfpe_recovery_func, sigfpe_decider_func, value);
  CHECK(shared.count_decider == 1);
  CHECK(shared.count_recovery == 1);

  /* Reset counters */
  shared.count_decider = 0;
  shared.count_recovery = 0;

  /* Second invocation - state should be properly reset */
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, sigfpe_func, sigfpe_recovery_func, sigfpe_decider_func, value);
  CHECK(shared.count_decider == 1);
  CHECK(shared.count_recovery == 1);

  /* Third invocation */
  shared.count_decider = 0;
  shared.count_recovery = 0;
  WG14_SIGNALS_PREFIX(sigguarded)
  (&guarded, sigfpe_func, sigfpe_recovery_func, sigfpe_decider_func, value);
  CHECK(shared.count_decider == 1);
  CHECK(shared.count_recovery == 1);
  return ret;
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

  /* Run all tests */
  ret += test_thread_local_sigfpe();
  ret += test_global_sigfpe();
  ret += test_sigfpe_with_custom_value();
  ret += test_multiple_sigfpe_invocations();

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  printf("Exiting main with result %d ...\n", ret);
  return ret;
}
