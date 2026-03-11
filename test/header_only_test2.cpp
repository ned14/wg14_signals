#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1
#include "wg14_signals/current_thread_id.h"

// Drag in everything else while we're at it
#include "wg14_signals/thrd_signal_handle.h"
#include "wg14_signals/tss_async_signal_safe.h"

extern WG14_SIGNALS_PREFIX(thread_id_t) test2()
{
  return WG14_SIGNALS_PREFIX(current_thread_id)();
}