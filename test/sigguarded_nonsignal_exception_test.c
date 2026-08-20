#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Regression test for analysis.md SIGM: the sigguarded() frame filter on
// Windows must not treat sigismember()'s -1 (signo outside 1..32) as
// membership. A non-signal exception -- the MSVC C++ /EHa exception code
// (0xE06D7363, maps to signo 0) or an application user-range RaiseException
// (0x40000000 | signo, maps to a bogus out-of-range signo) -- must skip the
// frame decider and propagate past the guard. Pre-fix the decider ran with
// rsi->signo == 0 or a bogus signo, so a decider returning call_recovery ran
// recovery for a non-signal and one returning resume_execution issued
// EXCEPTION_CONTINUE_EXECUTION on e.g. a C++ throw.
//
// sigguarded() is implemented with MSVC __try/__except on Windows
// (thrd_signal_handle_windows.c.ipp; MinGW deliberately has no SEH
// alternative), so the test wraps the guarded calls in an outer SEH handler
// that catches the escaped non-signal exceptions. The whole Windows-only body
// is guarded by _WIN32 and is a no-op on POSIX, where there is no Windows
// frame filter.
#ifdef _WIN32

static int frame_decider_calls = 0;

// Never the right answer for a non-signal exception, but a safe one for a
// genuine fault: either way the filter continues the exception search, so the
// exception reaches the outer handler below and the test checks how often the
// decider was invoked.
static enum WG14_SIGNALS_PREFIX(sig_decision)
declining_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  (void) rsi;
  frame_decider_calls++;
  return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

// A genuine guarded fault: EXCEPTION_ACCESS_VIOLATION maps to SIGSEGV.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
raise_guarded_fault(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, WG14_SIGNALS_NULLPTR);
  return value;
}

// The MSVC C++ /EHa exception code: signal_from_win32_exception_code() maps
// it to signo 0.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
raise_cpp_exception(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  RaiseException((DWORD) 0xE06D7363L, 0, 0, WG14_SIGNALS_NULLPTR);
  return value;
}

// An application user-range raise (the same range stdc_raise() uses for
// signals with no native Win32 code): 0x40000000 | 100 maps to signo 100,
// outside the 1..32 Windows sigset range.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
raise_user_range_code(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  RaiseException((DWORD) (0x40000000u | 100u), 0, 0, WG14_SIGNALS_NULLPTR);
  return value;
}

#endif

int main(void)
{
  int ret = 0;
#ifdef _WIN32
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGSEGV);
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};

  SECTION(
  "positive control: a guarded genuine fault still invokes the frame decider");
  frame_decider_calls = 0;
  __try
  {
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, raise_guarded_fault,
                                    noop_recovery, declining_decider, value);
    CHECK(0 && "guarded fault did not raise through the guard");
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
  }
  // The frame decider saw the genuine SIGSEGV exactly once and declined, so
  // the exception escaped to the outer handler. A regression in the fix that
  // lost the member path would fail this.
  CHECK(frame_decider_calls == 1);

  SECTION("non-signal C++ /EHa exception (signo 0) skips the frame decider");
  frame_decider_calls = 0;
  int cpp_exception_caught = 0;
  __try
  {
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, raise_cpp_exception,
                                    noop_recovery, declining_decider, value);
    CHECK(0 && "non-signal exception did not escape the guard");
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
    cpp_exception_caught = 1;
  }
  CHECK(cpp_exception_caught);
  // sigismember(guarded, 0) == -1, truthy pre-fix: the decider ran with
  // signo == 0. Post-fix it must not run at all.
  CHECK(frame_decider_calls == 0);

  SECTION(
  "user-range raise with an out-of-range signo skips the frame decider");
  frame_decider_calls = 0;
  int user_range_caught = 0;
  __try
  {
    WG14_SIGNALS_PREFIX(sigguarded)(&guarded, raise_user_range_code,
                                    noop_recovery, declining_decider, value);
    CHECK(0 && "non-signal exception did not escape the guard");
  }
  __except(EXCEPTION_EXECUTE_HANDLER)
  {
    user_range_caught = 1;
  }
  CHECK(user_range_caught);
  // sigismember(guarded, 100) == -1, truthy pre-fix: the decider ran with a
  // bogus signo. Post-fix it must not run at all.
  CHECK(frame_decider_calls == 0);
#endif
  return ret;
}
