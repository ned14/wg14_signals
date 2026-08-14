#include <signal.h>
#undef NSIG
#define NSIG 1024
#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <string.h>

// White-box regression test for analysis.md 2.21/Z1: the verstable-variant
// signo_to_sighandler_map_t (selected when NSIG >= 1024) was never
// initialised. sig_global_state() returns a zero-initialised static, so the
// table's metadata pointer was NULL and _get/_insert dereferenced it, crashing
// every map-touching library operation. No mainstream libc reaches NSIG >=
// 1024, so no CI leg exercises the branch; this TU forces it by overriding
// NSIG to 1024 (after <signal.h>, whose own NSIG is irrelevant to the map) and
// including the library in header-only mode, which makes the internal
// sig_global_state()/map functions per-TU statics callable from here.
int main(void)
{
  int ret = 0;

  // sig_global_state() must have initialised the verstable map: metadata must
  // point at the empty-bucket placeholder, not NULL (before the fix _get below
  // crashed on the NULL pointer).
  struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
  WG14_SIGNALS_PREFIX(sig_global_state)();
  CHECK(state->signo_to_sighandler_map.metadata != WG14_SIGNALS_NULLPTR);

  // _get on the (still empty) table must return an end iterator, not crash.
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGSEGV);
  CHECK(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it));

  // _insert/_get/_erase_itr round-trip must work on the initialised map.
  struct WG14_SIGNALS_PREFIX(sighandler_info) info;
  memset(&info, 0, sizeof(info));
  it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_insert)(
  &state->signo_to_sighandler_map, SIGSEGV, &info);
  CHECK(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it));
  it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGSEGV);
  CHECK(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it));
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)(
  &state->signo_to_sighandler_map, it);
  it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  &state->signo_to_sighandler_map, SIGSEGV);
  CHECK(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it));

  return ret;
}
