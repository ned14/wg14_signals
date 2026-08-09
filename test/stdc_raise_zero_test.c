#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

int main(void)
{
  int ret = 0;

  // The documented one-line library setup call
  // (thrd_signal_handle.h:341-346). Before analysis.md 1.4 was fixed, on
  // Windows this aborted the process: the Windows stdc_raise had no
  // signo == 0 short-circuit, so win32_exception_code_from_signal(0) executed
  // default: abort(). It must set up the calling thread's TLS state and return
  // false doing nothing else.
  CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR) == false);

  return ret;
}
