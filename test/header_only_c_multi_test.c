#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1
#include "wg14_signals/current_thread_id.h"
#include "wg14_signals/tss_async_signal_safe.h"

extern WG14_SIGNALS_PREFIX(thread_id_t) header_only_c_multi_test1(void);
extern WG14_SIGNALS_PREFIX(thread_id_t) header_only_c_multi_test2(void);

/* Multi-TU C header-only consumer (analysis.md Y10): three C translation
   units, each with its own per-TU static-inline copies, must link without
   duplicate symbols. */
int main(void)
{
  return (header_only_c_multi_test1() != 0 &&
          header_only_c_multi_test2() != 0) ?
         0 :
         1;
}
