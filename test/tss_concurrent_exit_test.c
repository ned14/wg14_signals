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

// Number of concurrent create/init/exit/destroy cycles.
#define CONCURRENT_ITERS 1000

static TSS tls;
static atomic_int concurrent_exit_barrier;

static int create_cb(void **dest)
{
  *dest = malloc(1);
  return (*dest != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}
static int destroy_cb(void *v)
{
  free(v);
  return 0;
}

static int thr_init_exit(void *unused)
{
  (void) unused;
  if(TSS_THREAD_INIT(tls) != 0)
  {
    return -1;
  }
  if(TSS_GET(tls) == WG14_SIGNALS_NULLPTR)
  {
    return -1;
  }
  return 0;
}

// Both worker threads arrive at the barrier before either returns, so their
// thread-exit deinit callbacks run at roughly the same time.
static int thr_init_barrier_exit(void *unused)
{
  (void) unused;
  atomic_fetch_add_explicit(&concurrent_exit_barrier, 1, memory_order_relaxed);
  if(TSS_THREAD_INIT(tls) != 0)
  {
    return -1;
  }
  if(TSS_GET(tls) == WG14_SIGNALS_NULLPTR)
  {
    return -1;
  }
  while(atomic_load_explicit(&concurrent_exit_barrier, memory_order_relaxed) <
        2)
  {
  }
  return 0;
}

// Regression for plans/analysis.md 2.17/2.18 (X1/X2): once the last registered
// thread has exited and its deinit has freed the shared deinit_state, neither a
// thread_init on a fresh thread nor a destroy may write through the freed
// pointer. Without the fix this is an ASan heap-use-after-free in
// tss_async_signal_safe_thread_init (WRITE of size 4 at :217) and in
// tss_async_signal_safe_destroy (WRITE of size 8 at :119).
static int test_reinit_and_destroy_after_all_exited(void)
{
  int ret = 0;
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  thrd_t t1, t2;
  int res = 0;
  CHECK(0 == TSS_CREATE(&tls, &attr));
  CHECK(0 == thrd_create(&t1, thr_init_exit, WG14_SIGNALS_NULLPTR));
  thrd_join(t1, &res);
  CHECK(res == 0);
  // T1's exit freed the deinit_state; T2's thread_init must reallocate it.
  CHECK(0 == thrd_create(&t2, thr_init_exit, WG14_SIGNALS_NULLPTR));
  thrd_join(t2, &res);
  CHECK(res == 0);
  // Destroy after the last registered thread exited (X2).
  CHECK(0 == TSS_DESTROY(tls));
  return ret;
}

// Regression for plans/analysis.md 2.1: two threads sharing the same
// tss_async_signal_safe exit at roughly the same time. The deinit count
// decrement and the deinit_state free must serialise on mem->lock so that no
// deinit frees the shared deinit_state while the other thread is still
// decrementing it or reading state->val.
static int test_concurrent_exit(void)
{
  int ret = 0;
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  for(int i = 0; i < CONCURRENT_ITERS; i++)
  {
    atomic_store_explicit(&concurrent_exit_barrier, 0, memory_order_relaxed);
    thrd_t a, b;
    int res = 0;
    CHECK(0 == TSS_CREATE(&tls, &attr));
    if(thrd_create(&a, thr_init_barrier_exit, WG14_SIGNALS_NULLPTR) != 0)
    {
      CHECK(0);
      continue;
    }
    if(thrd_create(&b, thr_init_barrier_exit, WG14_SIGNALS_NULLPTR) != 0)
    {
      CHECK(0);
      atomic_store_explicit(&concurrent_exit_barrier, 2, memory_order_relaxed);
      thrd_join(a, &res);
      continue;
    }
    thrd_join(a, &res);
    thrd_join(b, &res);
    CHECK(res == 0);
    CHECK(0 == TSS_DESTROY(tls));
  }
  return ret;
}

int main(void)
{
  int ret = 0;
  ret += test_reinit_and_destroy_after_all_exited();
  ret += test_concurrent_exit();
  printf("tss deinit_state lifetime regression checks passed\n");
  return ret;
}
