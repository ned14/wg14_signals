/* Proposed WG14 improved signals support
(C) 2024 Niall Douglas <http://www.nedproductions.biz/>
File Created: Nov 2024


Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License in the accompanying file
Licence.txt or at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef WG14_SIGNALS_TSS_ASYNC_SIGNAL_SAFE_IPP
#define WG14_SIGNALS_TSS_ASYNC_SIGNAL_SAFE_IPP

#include "../../tss_async_signal_safe.h"

#include "../../current_thread_id.h"

#include "lock_unlock.h"
#include "thread_atexit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
extern "C"
{
#else
#include <stdatomic.h>
#endif

// The map key is a composite of a per-thread generation counter (high 32 bits)
// and the kernel thread id (low 32 bits), so a thread id reused after a thread
// exited without running its exit-time deinit maps to a fresh key rather than
// the previous incarnation's stale entry (plans/analysis.md TIDR).
#define NAME WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t)
#define KEY_TY uint64_t
#define VAL_TY void *
#define HASH_FN vt_hash_integer
#define CMPR_FN vt_cmpr_integer
#include "verstable.h"
#undef CMPR_FN
#undef HASH_FN
#undef VAL_TY
#undef KEY_TY
#undef NAME

#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
#endif

  struct WG14_SIGNALS_PREFIX(deinit_state)
  {
#ifdef __cplusplus
    std::
#endif
    atomic_uint count;
    WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val;
  };
  struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr) attr;

#ifdef __cplusplus
    std::
#endif
    atomic_uint lock;
    struct WG14_SIGNALS_PREFIX(deinit_state) * state;
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t) thread_id_to_tls_map;
  };

  // Per-thread generation counter shared across all translation units (weak /
  // selectany so the linker merges the per-TU definitions of a header-only
  // build): the first library use on a thread draws a fresh generation from the
  // process-wide counter below, so a kernel thread id reused after a thread
  // exited without running its exit-time deinit still yields a map key no
  // previous incarnation used (plans/analysis.md TIDR). The kernel tid cache
  // below stays per-TU (it holds the kernel tid, which is the same value in
  // every TU); the generation cache must be shared, or two translation units
  // would hand the same generation to two different threads whose tids collide.
  // Async-safe TLS where the platform provides it (Linux/Windows), plain
  // _Thread_local elsewhere (Apple fallback): the generation is assigned from
  // outside the signal handler, so a handler-context read of an already-primed
  // cache is the same fast path the kernel tid cache already relies on
  // (plans/analysis.md 7.3/AA8).
#if WG14_SIGNALS_ENABLE_HEADER_ONLY || defined(_WIN32)
  WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS
#endif
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
  WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL
#else
WG14_SIGNALS_THREAD_LOCAL
#endif
  WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(tss_generation_cached) = 0;

  // A single shared definition of the process-wide generation counter for all
  // translation units (weak / selectany so the linker merges the per-TU
  // definitions of a header-only multi-TU build): every thread's first library
  // use draws on the same monotonic counter, so two threads can never share a
  // generation. The `{0}` / `0` split is because C++ `std::atomic` rejects
  // copy-initialisation from a scalar while C rejects brace-initialisation of
  // `_Atomic` scalars.
#if WG14_SIGNALS_ENABLE_HEADER_ONLY || defined(_WIN32)
  WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS
#endif
  WG14_SIGNALS_ATOMIC_PREFIX
  atomic_uintptr_t WG14_SIGNALS_PREFIX(tss_generation_counter) =
#ifdef __cplusplus
  {0};
#else
0;
#endif

  // Keep a local cache of the current thread id. Use the async-signal-safe
  // TLS attribute where the platform provides it (Linux/Windows: initial-exec
  // ELF TLS or MSVC TLS, both async-signal-safe with no __tls_get_addr trap on
  // first access); on platforms without it (Apple fallback) fall back to plain
  // _Thread_local, where the cache is primed from outside the signal handler
  // so the first handler-context access is the fast path (analysis.md 7.3/AA8).
  static uint64_t WG14_SIGNALS_PREFIX(my_current_thread_id)(void)
  {
#ifdef WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL
    static WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_id_t)
    current_thread_id_mycache;
#else
  static WG14_SIGNALS_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_id_t)
  current_thread_id_mycache;
#endif
    if(current_thread_id_mycache == WG14_SIGNALS_PREFIX(thread_id_t_tombstone))
    {
      current_thread_id_mycache = WG14_SIGNALS_PREFIX(current_thread_id)();
    }
    if(WG14_SIGNALS_PREFIX(tss_generation_cached) == 0)
    {
      // One-based: 0 is the tombstone meaning "no generation assigned yet".
      WG14_SIGNALS_PREFIX(tss_generation_cached) =
      (WG14_SIGNALS_PREFIX(thread_id_t))(
      1 + atomic_fetch_add_explicit(
          &WG14_SIGNALS_PREFIX(tss_generation_counter), 1,
          WG14_SIGNALS_ATOMIC_PREFIX memory_order_relaxed));
    }
    // The composite key: generation in the high half, kernel thread id in the
    // low half. Two threads always differ in generation, so the low-half
    // truncation cannot alias distinct tids with distinct generations.
    return ((uint64_t) WG14_SIGNALS_PREFIX(tss_generation_cached) << 32) |
           (uint64_t) (uint32_t) current_thread_id_mycache;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) * val,
  const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr) * attr)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) WG14_SIGNALS_CALLOC(
    1, sizeof(struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s)));
    if(mem == WG14_SIGNALS_NULLPTR)
    {
      return -1;
    }
    WG14_SIGNALS_MEMCPY(&mem->attr, attr, sizeof(mem->attr));
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_init)
    (&mem->thread_id_to_tls_map);
    *val = mem;
    return 0;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) val;
    LOCK(mem->lock);
    if(mem->state)
    {
      mem->state->val = WG14_SIGNALS_NULLPTR;
      mem->state = WG14_SIGNALS_NULLPTR;
    }
    // The user's attr.destroy callback must not run while mem->lock is held: a
    // documented-valid re-entrant call from the callback -- e.g.
    // tss_async_signal_safe_get() on the same handle, documented THREADSAFE
    // ASYNC-SIGNAL-SAFE -- would self-deadlock on the non-recursive spinlock
    // (plans/analysis.md UCLK, SPIN). Each value is therefore erased from the
    // map under the lock first -- so a re-entrant get() can never observe a
    // value whose destroy callback is running or has run -- and its callback is
    // invoked after the lock is released, mirroring
    // tss_async_signal_safe_thread_deinit. erase_itr() returns the next
    // iterator, the verstable-supported pattern for erasing while iterating.
    for(WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
        it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_first)(
        &mem->thread_id_to_tls_map);
        !WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it);)
    {
      void *item = it.data->val;
      it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_erase_itr)(
      &mem->thread_id_to_tls_map, it);
      UNLOCK(mem->lock);
      mem->attr.destroy(item);
      LOCK(mem->lock);
    }
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_cleanup)
    (&mem->thread_id_to_tls_map);
    UNLOCK(mem->lock);
    WG14_SIGNALS_FREE(mem);
    return 0;
  }

  static int WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_deinit)(
  struct WG14_SIGNALS_PREFIX(deinit_state) * state)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) state->val;
    int ret = 0;
    if(mem != WG14_SIGNALS_NULLPTR)
    {
      const uint64_t mytid = WG14_SIGNALS_PREFIX(my_current_thread_id)();
      LOCK(mem->lock);
      WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
      it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_get)(
      &mem->thread_id_to_tls_map, mytid);
      if(!WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
      {
        void *item = it.data->val;
        UNLOCK(mem->lock);
        ret = mem->attr.destroy(item);
        LOCK(mem->lock);
        WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_erase)
        (&mem->thread_id_to_tls_map, mytid);
      }
      // The count decrement and the state free must both hold mem->lock, or
      // two threads exiting concurrently could free state while the other
      // thread is still decrementing it (or reading state->val), and a later
      // thread_init/destroy would see a dangling mem->state. Only the last
      // registered thread frees state, and it must clear mem->state first so
      // that no later thread_init or destroy writes through the freed pointer.
      if(1 ==
         atomic_fetch_sub_explicit(
         &state->count, 1, WG14_SIGNALS_ATOMIC_PREFIX memory_order_relaxed))
      {
        if(state == mem->state)
        {
          mem->state = WG14_SIGNALS_NULLPTR;
        }
        WG14_SIGNALS_FREE(state);
      }
      UNLOCK(mem->lock);
    }
    else
    {
      // The instance was destroyed while this thread was still registered:
      // destroy() dropped its own pointer and cleared state->val but did not
      // free the deinit_state, so the reference this thread took at
      // thread_init() is still outstanding. Drop it now; the last registered
      // thread frees state.
      if(1 ==
         atomic_fetch_sub_explicit(
         &state->count, 1, WG14_SIGNALS_ATOMIC_PREFIX memory_order_relaxed))
      {
        WG14_SIGNALS_FREE(state);
      }
    }
    return ret;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) val;
    // This will force init the TLS from outside a signal handle
    const uint64_t mytid = WG14_SIGNALS_PREFIX(my_current_thread_id)();
    LOCK(mem->lock);
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
    it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_get)(
    &mem->thread_id_to_tls_map, mytid);
    int res = 0;
    if(WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
    {
      UNLOCK(mem->lock);
      void *newitem = WG14_SIGNALS_NULLPTR;
      int ret = mem->attr.create(&newitem);
      if(ret != 0)
      {
        // The create callback reported failure; propagate its error code.
        return ret;
      }
      if(newitem == WG14_SIGNALS_NULLPTR)
      {
        // A create callback that returns 0 but leaves *dest NULL is broken: it
        // would otherwise report success while no TID is inserted into the map,
        // and a later get() on this thread would return NULL indistinguishably
        // (plans/analysis.md 2.5). Report failure explicitly.
        errno = EINVAL;
        return -1;
      }
      LOCK(mem->lock);
      it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_insert)(
      &mem->thread_id_to_tls_map, mytid, newitem);
      if(WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
      {
        UNLOCK(mem->lock);
        mem->attr.destroy(newitem);
        errno = ENOMEM;
        return -1;
      }
      if(mem->state == WG14_SIGNALS_NULLPTR)
      {
        mem->state =
        (struct WG14_SIGNALS_PREFIX(deinit_state) *) WG14_SIGNALS_CALLOC(
        1, sizeof(struct WG14_SIGNALS_PREFIX(deinit_state)));
        if(mem->state == WG14_SIGNALS_NULLPTR)
        {
          WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_erase)
          (&mem->thread_id_to_tls_map, mytid);
          UNLOCK(mem->lock);
          mem->attr.destroy(newitem);
          errno = ENOMEM;
          return -1;
        }
        mem->state->val = val;
      }
      atomic_fetch_add_explicit(
      &mem->state->count, 1, WG14_SIGNALS_ATOMIC_PREFIX memory_order_relaxed);
      UNLOCK(mem->lock);
      void (*func)(void *) = (void (*)(void *))(uintptr_t) WG14_SIGNALS_PREFIX(
      tss_async_signal_safe_thread_deinit);
      res = WG14_SIGNALS_PREFIX(thread_atexit)(func, mem->state);
      return res;
    }
    UNLOCK(mem->lock);
    return res;
  }

  void *WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) val;
    const uint64_t mytid = WG14_SIGNALS_PREFIX(my_current_thread_id)();
    void *ret = WG14_SIGNALS_NULLPTR;
    LOCK(mem->lock);
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
    it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_get)(
    &mem->thread_id_to_tls_map, mytid);
    if(!WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
    {
      ret = it.data->val;
    }
    UNLOCK(mem->lock);
    return ret;
  }

#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
