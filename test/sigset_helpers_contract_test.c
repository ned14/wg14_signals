#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>

int main(void)
{
  int ret = 0;

  // VSDT: the N3924 synopses (7.14.2.1/7.14.2.2) follow the POSIX contract for
  // these APIs, so the documented `if(sigemptyset(&ss) != 0)` idiom must
  // compile and hold on every platform (plans/analysis.md VSDT). On Windows
  // pre-fix the four helpers returned void, so this comparison failed to
  // compile there; both backends now return 0 for valid input and -1 with
  // errno = EINVAL for an out-of-range signo in sigaddset/sigdelset/
  // sigismember, exactly like the POSIX host libc.
  sigset_t ss;
  CHECK(sigemptyset(&ss) == 0);
  CHECK(sigfillset(&ss) == 0);
  CHECK(sigaddset(&ss, SIGABRT) == 0);
  CHECK(sigdelset(&ss, SIGABRT) == 0);
  CHECK(sigismember(&ss, SIGABRT) == false);
  CHECK(sigaddset(&ss, SIGABRT) == 0);
  CHECK(sigismember(&ss, SIGABRT) == true);
  CHECK(sigdelset(&ss, SIGABRT) == 0);

  // POSIX imperative: out-of-range signo fails with -1 and errno = EINVAL.
  // Asserted only where the reference implementation owns the helpers (the
  // Windows in-tree versions). On POSIX they are the host libc's: glibc/musl
  // follow the same contract, but the macOS/BSD libcs deviate (sigaddset/
  // sigdelset return 0 without errno; sigismember is a shift-count macro,
  // undefined behaviour out of range) -- documented at
  // thrd_signal_handle.h:41-53 (plans/analysis.md 3).
#if defined(_WIN32)
  errno = 0;
  CHECK(sigaddset(&ss, 0) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(sigdelset(&ss, 0) == -1);
  CHECK(errno == EINVAL);
  errno = 0;
  CHECK(sigismember(&ss, 0) == -1);
  CHECK(errno == EINVAL);
#endif

  printf("sigset helper return-contract checks passed\n");
  return ret;
}
