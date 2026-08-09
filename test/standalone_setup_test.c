#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <signal.h>

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  return value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
recovery_func(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

static enum WG14_SIGNALS_PREFIX(sig_decision_t)
decider_func(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  return WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery);
}

int main(void)
{
  int ret = 0;

  // The documented one-line setup call. On fallback-TLS platforms (macOS and
  // any non-GNU/MSVC platform) this dereferenced the never-created NULL TSS
  // handle inside tss_async_signal_safe_thread_init() and crashed with a
  // SIGSEGV before analysis.md 1.3 was fixed; on Windows it aborted the
  // process before analysis.md 1.4 was fixed.
  WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                  WG14_SIGNALS_NULLPTR);

  // A bare sigguarded() with no prior siginstall() must not crash either.
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGABRT);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 42};
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) retv = WG14_SIGNALS_PREFIX(
  sigguarded)(&guarded, guarded_func, recovery_func, decider_func, value);
  CHECK(retv.int_value == 42);

  // The TSS state now exists; a further setup call must still not crash.
  WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                  WG14_SIGNALS_NULLPTR);

  return ret;
}
