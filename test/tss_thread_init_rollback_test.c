/* White-box regression test for 7.30.6.7 p2: when tss_async_signal_safe_
   thread_init() cannot complete the creation after the create callback has
   returned zero (here, thread_atexit() cannot allocate its registration node),
   it must invoke the destroy callback with the stored pointer and return
   thrd_error, leaving no map entry or deinit state behind.

   Header-only mode compiles tss_async_signal_safe.c.ipp and the pthread-key
   thread_atexit() fallback into this TU, so the failing allocator below is seen
   by thread_atexit(). The __cxa_thread_atexit() path is excluded because it
   deliberately ignores its own failure return. */

#include <stdlib.h>

static void *failing_malloc(size_t n)
{
  (void) n;
  return NULL;
}
#define WG14_SIGNALS_MALLOC(n) failing_malloc(n)

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1
#undef WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT

#include "test_common.h"

#include "wg14_signals/tss_async_signal_safe.h"

#define TSS WG14_SIGNALS_PREFIX(tss_async_signal_safe_t)
#define TSS_ATTR WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
#define TSS_CREATE WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)
#define TSS_DESTROY WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)
#define TSS_THREAD_INIT WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)

static int create_unit(void **dest)
{
  static int value;
  *dest = &value;
  return 0;
}

static int destroy_calls;
static int destroy_unit(void *v)
{
  (void) v;
  destroy_calls++;
  return 0;
}

int main(void)
{
  int ret = 0;
  TSS tls = WG14_SIGNALS_NULLPTR;
  struct TSS_ATTR attr = {create_unit, destroy_unit};
  struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) * mem;

  CHECK(thrd_success == TSS_CREATE(&tls, &attr));
  CHECK(thrd_error == TSS_THREAD_INIT(tls));
  CHECK(destroy_calls == 1);

  // The failed initialisation must have erased the map entry and released the
  // deinit state it allocated for this thread.
  mem = (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) tls;
  CHECK(mem->state == WG14_SIGNALS_NULLPTR);

  // Destroying the handle must not invoke the destroy callback a second time
  // for the entry the failed thread_init rolled back.
  CHECK(thrd_success == TSS_DESTROY(tls));
  CHECK(destroy_calls == 1);

  return ret;
}
