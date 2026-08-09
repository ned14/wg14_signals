#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1
#include "wg14_signals/current_thread_id.h"
#include "wg14_signals/tss_async_signal_safe.h"

WG14_SIGNALS_PREFIX(thread_id_t) header_only_c_multi_test1(void)
{
  return WG14_SIGNALS_PREFIX(current_thread_id)();
}
