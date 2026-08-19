#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1
#include "wg14_signals/current_thread_id.h"
#include "wg14_signals/thrd_signal_handle.h"
#include "wg14_signals/tss_async_signal_safe.h"

#include <stdlib.h>

/* A single-TU C header-only consumer. Every library symbol is a per-TU static
   inline in header-only mode (analysis.md 1.8/C3), so this must build, link
   and run without linking the library. Compiled by
   test/confirm_header_only_broken.cmake. */
static int tls_create(void **dest)
{
  *dest = malloc(1);
  return (*dest != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}
static int tls_destroy(void *v)
{
  free(v);
  return 0;
}

int main(void)
{
  const WG14_SIGNALS_PREFIX(thread_id_t) tid =
  WG14_SIGNALS_PREFIX(current_thread_id)();
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) tls = WG14_SIGNALS_NULLPTR;
  struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
  attr = {tls_create, tls_destroy};
  WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                  WG14_SIGNALS_NULLPTR);
  if(0 != WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(&tls, &attr))
  {
    fprintf(stderr, "header_only_c_consumer: tss_async_signal_safe_create "
                    "failed\n");
    return 1;
  }
  if(0 != WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(tls))
  {
    fprintf(stderr, "header_only_c_consumer: tss_async_signal_safe_thread_init "
                    "failed\n");
    return 1;
  }
  if(WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(tls) ==
     WG14_SIGNALS_NULLPTR)
  {
    fprintf(stderr, "header_only_c_consumer: tss_async_signal_safe_get "
                    "returned NULL\n");
    return 1;
  }
  if(0 != WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(tls))
  {
    fprintf(stderr, "header_only_c_consumer: tss_async_signal_safe_destroy "
                    "failed\n");
    return 1;
  }
  if(tid == 0)
  {
    fprintf(stderr, "header_only_c_consumer: current_thread_id() returned 0\n");
    return 1;
  }
  return 0;
}
