#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

struct shared_t
{
  int count;
};

static bool
sigill_decider_func(struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info) * rsi)
{
  struct shared_t *shared = (struct shared_t *) rsi->value.ptr_value;
  shared->count++;
  return true;  // handled
}

int main()
{
  int ret = 0;
  void *handlers =
  WG14_SIGNALS_PREFIX(modern_signals_install)(WG14_SIGNALS_NULLPTR, 0);

  {
    struct shared_t shared = {.count = 0};
    union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)
    value = {.ptr_value = &shared};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGILL);
    void *sigill_decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, sigill_decider_func, value);
    CHECK(WG14_SIGNALS_PREFIX(thrd_signal_raise)(SIGILL, WG14_SIGNALS_NULLPTR,
                                                 WG14_SIGNALS_NULLPTR));
    CHECK(shared.count == 1);
    WG14_SIGNALS_PREFIX(signal_decider_destroy(sigill_decider));
  }

  CHECK(WG14_SIGNALS_PREFIX(modern_signals_uninstall)(handlers));
  printf("Exiting main with result %d ...\n", ret);
  return ret;
}