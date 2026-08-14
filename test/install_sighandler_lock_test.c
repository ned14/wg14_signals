// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): the .ipp
// implementations compiled below must see the same declarations as the compiled
// library. _GNU_SOURCE alone is the mirror — on glibc it subsumes the library's
// _POSIX_C_SOURCE/_XOPEN_SOURCE trio, and macOS/BSD must not take those at all
// (they hide NSIG, which the signo-to-sighandler map needs, finding `NSIG`).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// White-box regression test for analysis.md 2.20/Y1: install_sighandler()
// returned false while still holding the global state->lock when the backend
// install failed, permanently deadlocking every subsequent library call.
//
// Including the header with WG14_SIGNALS_ENABLE_HEADER_ONLY makes the internal
// install_sighandler() and sig_global_state() per-TU statics callable from here
// (the header_only_c_multi_test pattern, ideas.md 10). SIGKILL is the one
// signal the siginstall() loop skips for which sigaction() is *guaranteed* to
// fail (EINVAL) on every POSIX platform, so calling install_sighandler(SIGKILL)
// deterministically drives install_sighandler_impl() to failure.
#ifndef _WIN32
static int whitebox_install_failure_releases_lock(void)
{
  int ret = 0;
  // install_sighandler(SIGKILL) -> sigaction(SIGKILL) == -1 -> the
  // install_sighandler_impl() failure branch of install_sighandler(). Before
  // the fix that branch returned false with state->lock still held.
  CHECK(WG14_SIGNALS_PREFIX(install_sighandler)(SIGKILL) == false);
  // A fresh acquisition of the global lock must not spin: it spins forever
  // before the fix, so this is the regression check.
  struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
  WG14_SIGNALS_PREFIX(sig_global_state)();
  LOCK(state->lock);
  UNLOCK(state->lock);
  return ret;
}
#endif

int main(void)
{
  int ret = 0;

#ifndef _WIN32
  ret += whitebox_install_failure_releases_lock();
#endif

  // The library must remain fully usable after a failed install attempt.
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
