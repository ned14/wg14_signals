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

#define NAME WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t)
#define KEY_TY WG14_SIGNALS_PREFIX(thread_id_t)
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
    WG14_SIGNALS_PREFIX(tss_async_signal_safe) val;
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

  // Keep a local cache of the current thread id, if thread locals aren't async
  // signal safe on this platform it doesn't matter as we'll ensure it is
  // initialised from outside the signal handler
  static WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(my_current_thread_id)(void)
  {
    static WG14_SIGNALS_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_id_t)
    current_thread_id_mycache;
    if(current_thread_id_mycache == WG14_SIGNALS_PREFIX(thread_id_t_tombstone))
    {
      current_thread_id_mycache = WG14_SIGNALS_PREFIX(current_thread_id)();
    }
    return current_thread_id_mycache;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe) * val,
  const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr) * attr)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) calloc(
    1, sizeof(struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s)));
    if(mem == WG14_SIGNALS_NULLPTR)
    {
      return -1;
    }
    memcpy(&mem->attr, attr, sizeof(mem->attr));
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_init)
    (&mem->thread_id_to_tls_map);
    *val = mem;
    return 0;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe) val)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) val;
    LOCK(mem->lock);
    if(mem->state)
    {
      mem->state->val = WG14_SIGNALS_NULLPTR;
      mem->state = WG14_SIGNALS_NULLPTR;
    }
    for(WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
        it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_first)(
        &mem->thread_id_to_tls_map);
        !WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it);
        it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_next)(it))
    {
      mem->attr.destroy(it.data->val);
    }
    WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_cleanup)
    (&mem->thread_id_to_tls_map);
    free(mem);
    return 0;
  }

  static int WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_deinit)(
  struct WG14_SIGNALS_PREFIX(deinit_state) * state)
  {
    struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *mem =
    (struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *) state->val;
    if(mem != WG14_SIGNALS_NULLPTR)
    {
      const uint64_t mytid = WG14_SIGNALS_PREFIX(my_current_thread_id)();
      LOCK(mem->lock);
      WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_itr)
      it = WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_get)(
      &mem->thread_id_to_tls_map, mytid);
      if(!WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_is_end)(it))
      {
        UNLOCK(mem->lock);
        int ret = mem->attr.destroy(it.data->val);
        if(ret != 0)
        {
          return ret;
        }
        LOCK(mem->lock);
        WG14_SIGNALS_PREFIX(thread_id_to_tls_map_t_erase)
        (&mem->thread_id_to_tls_map, mytid);
      }
      UNLOCK(mem->lock);
    }
    if(1 == atomic_fetch_sub_explicit(
            &state->count, 1, WG14_SIGNALS_ATOMIC_PREFIX memory_order_relaxed))
    {
      free(state);
    }
    return 0;
  }

  int WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe) val)
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
      if(ret != 0 || newitem == WG14_SIGNALS_NULLPTR)
      {
        return ret;
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
        mem->state = (struct WG14_SIGNALS_PREFIX(deinit_state) *) calloc(
        1, sizeof(struct WG14_SIGNALS_PREFIX(deinit_state)));
        if(mem->state == WG14_SIGNALS_NULLPTR)
        {
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
  WG14_SIGNALS_PREFIX(tss_async_signal_safe) val)
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
