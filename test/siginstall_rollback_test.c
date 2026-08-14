// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): the .ipp
// implementations compiled below must see the same declarations as the compiled
// library. _GNU_SOURCE alone is the mirror — on glibc it subsumes the library's
// _POSIX_C_SOURCE/_XOPEN_SOURCE trio, and macOS/BSD must not take those at all
// (they hide NSIG, which the signo-to-sighandler map needs, analysis.md Y6).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

// White-box regression test for analysis.md 3.8: a siginstall() whose loop
// fails part-way must roll back the signals already installed in that call,
// so no handler is left installed with no handle to uninstall it and a
// subsequent siginstall does not double-count.
//
// install_sighandler() fails deterministically when its per-signal
// sighandler_info allocation fails, so this test links with --wrap=calloc and
// fails the sighandler_info-sized calloc for a chosen later signal. The
// header-only build compiles the .ipp into this TU, so the interposer sees the
// .ipp's calloc calls and can size-match the struct.
#ifndef _WIN32
// Number of sighandler_info allocations to allow before failing. Negative
// means "never fail" (the normal install must not trip it).
static int sighandler_info_calls_allowed = -1;
static int sighandler_info_calls = 0;
void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size)
{
  if(size == sizeof(struct WG14_SIGNALS_PREFIX(sighandler_info)))
  {
    sighandler_info_calls++;
    if(sighandler_info_calls_allowed >= 0 &&
       sighandler_info_calls > sighandler_info_calls_allowed)
    {
      errno = ENOMEM;
      return WG14_SIGNALS_NULLPTR;
    }
  }
  return __real_calloc(nmemb, size);
}

static int whitebox_partial_install_rolls_back(void)
{
  int ret = 0;
  struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
  WG14_SIGNALS_PREFIX(sig_global_state)();

  // Allow the first signal's sighandler_info calloc, fail the second.
  sighandler_info_calls = 0;
  sighandler_info_calls_allowed = 1;

  sigset_t guarded;
  sigemptyset(&guarded);
  // Two signals: the first installs successfully, the second's calloc fails,
  // and siginstall must then uninstall the first before returning NULL.
  sigaddset(&guarded, SIGUSR1);
  sigaddset(&guarded, SIGUSR2);
  errno = 0;
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers == WG14_SIGNALS_NULLPTR);
  CHECK(errno == ENOMEM);

  // Rollback must have removed every handler this call installed: the global
  // handler count is back to zero and no map entry remains for either signal.
  LOCK(state->lock);
  CHECK(state->sighandlers_count == 0);
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  it1 = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGUSR1);
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  it2 = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGUSR2);
  CHECK(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it1));
  CHECK(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it2));
  UNLOCK(state->lock);

  // A subsequent full siginstall must work cleanly (no double-counting), and
  // its uninstall must leave the state pristine.
  sighandler_info_calls_allowed = -1;
  handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
  LOCK(state->lock);
  CHECK(state->sighandlers_count == 0);
  UNLOCK(state->lock);

  return ret;
}
#endif

int main(void)
{
  int ret = 0;

#ifndef _WIN32
  ret += whitebox_partial_install_rolls_back();
#endif

  // The library must remain fully usable after a failed install attempt.
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

  return ret;
}
