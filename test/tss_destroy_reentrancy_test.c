#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/tss_async_signal_safe.h"

#include <stdatomic.h>
#include <stdlib.h>

#define TSS WG14_SIGNALS_PREFIX(tss_async_signal_safe)
#define TSS_ATTR WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
#define TSS_CREATE WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)
#define TSS_DESTROY WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)
#define TSS_THREAD_INIT WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)
#define TSS_GET WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)

// Regression test for plans/analysis.md UCLK: tss_async_signal_safe_destroy
// must run the user's attr.destroy callback WITHOUT holding mem->lock. A
// documented-valid re-entrant library call from the callback -- here the
// THREADSAFE ASYNC-SIGNAL-SAFE get() on the same handle -- would pre-fix
// self-deadlock on the non-recursive spinlock (the SPIN mechanism); the test
// then hangs and is caught by the ctest timeout below.

static TSS g_tls;
static int destroy_calls = 0;
static int get_never_returned_being_destroyed = 1;

static int create_cb(void **dest)
{
  unsigned *v = (unsigned *) malloc(sizeof(unsigned));
  if(v == WG14_SIGNALS_NULLPTR)
  {
    return -1;
  }
  *v = 42;
  *dest = v;
  return 0;
}
static int destroy_cb(void *v)
{
  // Re-entrant get() on the same handle mid-destroy: must not deadlock, and
  // must never return the value whose destroy callback is running (that value
  // was erased from the map before its callback ran, so get() can only see
  // NULL or a still-live sibling whose own callback has not run yet).
  if(TSS_GET(g_tls) == v)
  {
    get_never_returned_being_destroyed = 0;
  }
  destroy_calls++;
  free(v);
  return 0;
}

static atomic_int worker_ready;
static atomic_int destroy_done;

static int worker_thr(void *unused)
{
  (void) unused;
  const int init_ret = TSS_THREAD_INIT(g_tls);
  atomic_store_explicit(&worker_ready, 1, memory_order_release);
  if(init_ret != 0)
  {
    return -1;
  }
  // Stay registered (do not run the exit-time deinit, which would erase this
  // thread's map entry) until the main thread has destroyed the handle, so
  // destroy() iterates two live entries.
  while(atomic_load_explicit(&destroy_done, memory_order_acquire) == 0)
  {
  }
  return 0;
}

int main(void)
{
  int ret = 0;
  atomic_store_explicit(&worker_ready, 0, memory_order_relaxed);
  atomic_store_explicit(&destroy_done, 0, memory_order_relaxed);
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  CHECK(0 == TSS_CREATE(&g_tls, &attr));
  CHECK(0 == TSS_THREAD_INIT(g_tls));
  unsigned *main_val = (unsigned *) TSS_GET(g_tls);
  CHECK(main_val != WG14_SIGNALS_NULLPTR);
  CHECK(*main_val == 42);

  thrd_t thr;
  int res = 0;
  CHECK(thrd_success == thrd_create(&thr, worker_thr, WG14_SIGNALS_NULLPTR));
  // Wait until the worker's entry is registered: the worker's release store of
  // worker_ready happens-after its thread_init, so this acquire load
  // synchronises with it (no sleeps, plans/AGENTS.md rule 5).
  while(atomic_load_explicit(&worker_ready, memory_order_acquire) == 0)
  {
  }
  // Two entries are live; destroy runs both attr.destroy callbacks with the
  // lock released, and each callback re-enters get() on the same handle.
  CHECK(0 == TSS_DESTROY(g_tls));
  CHECK(destroy_calls == 2);
  CHECK(get_never_returned_being_destroyed);
  atomic_store_explicit(&destroy_done, 1, memory_order_release);
  thrd_join(thr, &res);
  CHECK(res == 0);

  printf("destroy-callback re-entrancy checks passed\n");
  return ret;
}
