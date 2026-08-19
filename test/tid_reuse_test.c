// Feature-test-macro mirror of the library build (plans/ideas.md 2.2): the
// header-only .ipp implementation compiled below must see the same declarations
// as the compiled library. _GNU_SOURCE alone is the mirror -- on glibc it
// subsumes the library's _POSIX_C_SOURCE/_XOPEN_SOURCE trio, and macOS/BSD must
// not take those at all (they hide NSIG).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1

#include "test_common.h"

#include "wg14_signals/tss_async_signal_safe.h"

#include <stdlib.h>

#define TSS WG14_SIGNALS_PREFIX(tss_async_signal_safe_t)
#define TSS_ATTR WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
#define TSS_CREATE WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)
#define TSS_DESTROY WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)
#define TSS_THREAD_INIT WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)
#define TSS_GET WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)

// White-box regression test for plans/analysis.md TIDR: the
// tss_async_signal_safe map is keyed by a composite of a per-thread generation
// (high 32 bits) and the kernel thread id (low 32 bits). A thread that exits
// without running its exit-time deinit (abnormal termination, a cancelled
// thread_atexit registration, or the Darwin pthread-key fallback dropping every
// callback) leaves its entry in the map; a later thread that reuses the same
// kernel TID draws a different generation and so must NOT observe or inherit
// that stale entry -- it gets NULL from get() before initialising and a fresh
// value from thread_init().
//
// The stale entry is manufactured white-box: header-only mode compiles
// tss_async_signal_safe.c.ipp into this TU, so the internal map, the shared
// per-thread generation cache and the composite-key helper are reachable. The
// current thread's generation cache is pinned to an "old" generation and an
// entry for (old_generation, current_tid) is inserted directly -- the state a
// previous incarnation of this TID that never ran its deinit would leave. The
// generation cache is then moved to a "new" generation, the position a fresh
// thread reusing this TID is in, and the map must miss the stale entry.

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
  free(v);
  return 0;
}

// The first library use on a thread must assign a nonzero generation and keep
// it stable across idempotent re-initialisations (the documented "safe to call
// many times" contract), and the shared counter must advance so two threads
// can never share a generation.
static int test_generation_assignment(void)
{
  int ret = 0;
  WG14_SIGNALS_PREFIX(tss_generation_cached) = 0;
  TSS tls = WG14_SIGNALS_NULLPTR;
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  CHECK(0 == TSS_CREATE(&tls, &attr));
  CHECK(0 == TSS_THREAD_INIT(tls));
  const WG14_SIGNALS_PREFIX(thread_id_t) generation =
  WG14_SIGNALS_PREFIX(tss_generation_cached);
  CHECK(generation != 0);
  CHECK(0 == TSS_THREAD_INIT(tls));
  CHECK(WG14_SIGNALS_PREFIX(tss_generation_cached) == generation);
  CHECK(0 == TSS_DESTROY(tls));
  return ret;
}

// The core TIDR regression: a stale entry for this kernel tid under an old
// generation must be invisible to (and never reused by) a later incarnation of
// the tid.
static int test_stale_entry_not_observed(void)
{
  int ret = 0;
  WG14_SIGNALS_PREFIX(tss_generation_cached) = 1;
  TSS tls = WG14_SIGNALS_NULLPTR;
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  CHECK(0 == TSS_CREATE(&tls, &attr));
  struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
  (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) tls;

  // Manufacture the previous incarnation's entry: generation 1 keyed with this
  // thread's kernel tid, simulating a thread that exited without running its
  // deinit (the entry is never erased, and the value is never destroyed until
  // destroy() sweeps the map).
  const uint64_t stale_key = WG14_SIGNALS_PREFIX(my_current_thread_id)();
  unsigned *stale_val = (unsigned *) malloc(sizeof(unsigned));
  CHECK(stale_val != WG14_SIGNALS_NULLPTR);
  *stale_val = 7;
  LOCK(mem->lock);
  WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
  it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_insert)(
  &mem->thread_id_to_tls_map, stale_key, stale_val);
  CHECK(!WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it));
  UNLOCK(mem->lock);

  // Simulate the kernel reusing this tid for a new thread incarnation: the
  // generation cache moves to a fresh generation. Pre-fix, the map was keyed
  // by the bare kernel tid, so the new incarnation found (and reused) the
  // stale entry; post-fix the composite keys differ.
  WG14_SIGNALS_PREFIX(tss_generation_cached) = 2;
  CHECK(WG14_SIGNALS_PREFIX(my_current_thread_id)() != stale_key);

  // get() before initialisation must not observe the previous incarnation's
  // value.
  CHECK(TSS_GET(tls) == WG14_SIGNALS_NULLPTR);

  // thread_init() must create a fresh value for the new incarnation rather
  // than silently reuse the stale one.
  CHECK(0 == TSS_THREAD_INIT(tls));
  unsigned *val = (unsigned *) TSS_GET(tls);
  CHECK(val != WG14_SIGNALS_NULLPTR);
  CHECK(val != stale_val);
  CHECK(*val == 42);

  // destroy() sweeps both entries (the stale one and the live one), so no
  // value survives destroy.
  CHECK(0 == TSS_DESTROY(tls));
  return ret;
}

static TSS shared_tls;
static WG14_SIGNALS_PREFIX(thread_id_t) worker_generation;

static int worker_thr(void *unused)
{
  (void) unused;
  if(TSS_THREAD_INIT(shared_tls) != 0)
  {
    return -1;
  }
  worker_generation = WG14_SIGNALS_PREFIX(tss_generation_cached);
  return 0;
}

// Two threads' first uses must draw distinct generations from the shared
// counter, so their map entries can never alias even when the kernel reuses a
// tid between them.
static int test_distinct_generations(void)
{
  int ret = 0;
  WG14_SIGNALS_PREFIX(tss_generation_cached) = 0;
  struct TSS_ATTR attr = {.create = create_cb, .destroy = destroy_cb};
  CHECK(0 == TSS_CREATE(&shared_tls, &attr));
  CHECK(0 == TSS_THREAD_INIT(shared_tls));
  const WG14_SIGNALS_PREFIX(thread_id_t) main_generation =
  WG14_SIGNALS_PREFIX(tss_generation_cached);
  thrd_t thr;
  int res = 0;
  // Compare against thrd_success, not the literal 0: glibc defines thrd_success
  // as 0 but FreeBSD's <threads.h> defines it as 4.
  CHECK(thrd_success == thrd_create(&thr, worker_thr, WG14_SIGNALS_NULLPTR));
  thrd_join(thr, &res);
  CHECK(res == 0);
  CHECK(worker_generation != main_generation);
  CHECK(0 == TSS_DESTROY(shared_tls));
  return ret;
}

int main(void)
{
  int ret = 0;
  ret += test_generation_assignment();
  ret += test_stale_entry_not_observed();
  ret += test_distinct_generations();
  printf("tid-reuse generation checks passed\n");
  return ret;
}
