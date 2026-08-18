#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

int main(void)
{
  int ret = 0;

  // VSDT: the N3924 synopses (7.14.2.1/7.14.2.2) declare
  // sigemptyset/sigfillset/sigaddset/sigdelset as `int` "always returns zero",
  // so the documented `if(sigemptyset(&ss) != 0)` idiom must compile and hold
  // on every platform (plans/analysis.md VSDT). On Windows pre-fix the four
  // helpers returned void, so this comparison failed to compile there; on
  // POSIX these are the host libc's `int` functions, which return 0 for valid
  // input (out-of-range sigaddset/sigdelset are the documented POSIX
  // divergence from "always returns zero"). sigismember's bool/int satisfies
  // "positive one if set, zero if not".
  sigset_t ss;
  CHECK(sigemptyset(&ss) == 0);
  CHECK(sigfillset(&ss) == 0);
  CHECK(sigaddset(&ss, SIGABRT) == 0);
  CHECK(sigdelset(&ss, SIGABRT) == 0);
  CHECK(sigismember(&ss, SIGABRT) == false);
  CHECK(sigaddset(&ss, SIGABRT) == 0);
  CHECK(sigismember(&ss, SIGABRT) == true);
  CHECK(sigdelset(&ss, SIGABRT) == 0);

  printf("sigset helper return-contract checks passed\n");
  return ret;
}
