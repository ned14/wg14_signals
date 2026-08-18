#define _CRT_SECURE_NO_WARNINGS 1

// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): on glibc
// _GNU_SOURCE subsumes the library's _POSIX_C_SOURCE/_XOPEN_SOURCE trio. This
// TU does not force NSIG, so it compiles the regular array
// signo_to_sighandler_map_t.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <string.h>

// White-box regression test for plans/analysis.md UNKN: raw_signal_handler's
// fallback for a signal the library has no map entry for (stdc_raise() returns
// false) used to reset the kernel handler to SIG_DFL *before* taking the
// default action, silently discarding the library's installed handler — even
// for a default-ignore signal (SIGCHLD) where nothing is then re-raised. The
// fallback must now go through invoke_sigaction()'s SIG_DFL branch (which
// restores the current handler), so the installed handler survives the pass-on.
//
// The "unknown signal" state is manufactured white-box (header-only mode makes
// the internal sig_global_state()/map statics callable from this TU): SIGCHLD
// is installed, then its map entry is erased under the state lock while the
// kernel handler (the library's raw_signal_handler) is left installed — the
// transient window a concurrent install/uninstall would produce. Raising
// SIGCHLD then runs the fallback; its default action is ignore, so the process
// is not terminated, and the kernel handler must still be the library's
// afterwards.
int main(void)
{
  int ret = 0;
#ifdef _WIN32
  // raw_signal_handler is POSIX-only (analysis UNKN).
  return ret;
#else
  sigset_t guarded;
  sigemptyset(&guarded);
  sigaddset(&guarded, SIGCHLD);
  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "siginstall() failed: %s\n", strerror(errno));
    return ret;
  }
  struct sigaction before;
  memset(&before, 0, sizeof(before));
  CHECK(0 == sigaction(SIGCHLD, WG14_SIGNALS_NULLPTR, &before));
  // The library's handler must be installed for SIGCHLD before the raise.
  CHECK(before.sa_handler != SIG_DFL);
  CHECK(before.sa_handler != SIG_IGN);
  // Manufacture the transient "unknown signal" window: drop SIGCHLD from the
  // global map under the state lock, leaving the kernel handler installed.
  struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
  WG14_SIGNALS_PREFIX(sig_global_state)();
  struct WG14_SIGNALS_PREFIX(sighandler_info) *removed = WG14_SIGNALS_NULLPTR;
  LOCK(state->lock);
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGCHLD);
  CHECK(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it));
  if(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
  {
    removed = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_value)(it);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)(
    &state->signo_to_sighandler_map, it);
  }
  UNLOCK(state->lock);
  // Deliver SIGCHLD while the map has no entry for it: raw_signal_handler's
  // fallback runs. The default action is ignore, so the process is not
  // terminated; the delivery completes during the raising syscall's return to
  // user mode, i.e. before raise() returns.
  CHECK(removed != WG14_SIGNALS_NULLPTR);
  CHECK(0 == raise(SIGCHLD));
  struct sigaction after;
  memset(&after, 0, sizeof(after));
  CHECK(0 == sigaction(SIGCHLD, WG14_SIGNALS_NULLPTR, &after));
  // The fallback must NOT have permanently reset the kernel handler to SIG_DFL
  // (analysis UNKN): the library's handler survives the pass-on.
  CHECK(after.sa_handler != SIG_DFL);
  CHECK(after.sa_handler != SIG_IGN);
  CHECK(after.sa_handler == before.sa_handler);
  // Re-insert SIGCHLD so siguninstall() below uninstalls it properly (restoring
  // the kernel handler and freeing the container) instead of leaving the
  // library's handler installed for the test process to exit under.
  LOCK(state->lock);
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  it2 = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_insert)(
  &state->signo_to_sighandler_map, SIGCHLD, removed);
  CHECK(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it2));
  UNLOCK(state->lock);
  CHECK(0 == WG14_SIGNALS_PREFIX(siguninstall)(handlers));
  printf("unknown-signal fallback preserves the installed handler\n");
  return ret;
#endif
}
