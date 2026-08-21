/* Proposed WG14 improved signals support
(C) 2025 Niall Douglas <http://www.nedproductions.biz/>
File Created: Feb 2025


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

#ifndef WG14_SIGNALS_THRD_SIGNAL_HANDLE_COMMON_IPP
#define WG14_SIGNALS_THRD_SIGNAL_HANDLE_COMMON_IPP

#include "../../config.h"
#include "../../thrd_signal_handle.h"

#include "linked_list.h"
#include "lock_unlock.h"

#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
#include "thread_atexit.h"
#else
#include "../../tss_async_signal_safe.h"
#endif

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef __FILC__
#include <stdfil.h>
#endif

#ifdef __cplusplus
#include <atomic>
extern "C"
{
#else
#include <stdatomic.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // structure was padded
#endif

  struct WG14_SIGNALS_PREFIX(sighandler_info);

#if NSIG < 1024
  typedef struct WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t)
  {
    struct WG14_SIGNALS_PREFIX(sighandler_info) * arr[NSIG];
  } WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t);

  struct WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr_data)
  {
    struct WG14_SIGNALS_PREFIX(sighandler_info) * val;
    size_t idx;
  };
  typedef struct WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  {
    struct WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr_data) data_;
  } WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr);

#define signo_to_sighandler_map_t_value(x) (x).data_.val

  static inline WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t) * map, int idx)
  {
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr) ret;
    if(idx < 0 || idx >= NSIG || map->arr[idx] == WG14_SIGNALS_NULLPTR)
    {
      ret.data_.val = WG14_SIGNALS_NULLPTR;
      ret.data_.idx = (size_t) -1;
      return ret;
    }
    ret.data_.val = map->arr[idx];
    ret.data_.idx = idx;
    return ret;
  }
  static inline bool WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr) it)
  {
    return it.data_.val == WG14_SIGNALS_NULLPTR;
  }
  static inline WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_insert)(
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t) * map, int idx,
  struct WG14_SIGNALS_PREFIX(sighandler_info) * val)
  {
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr) ret;
    if(idx < 0 || idx >= NSIG)
    {
      ret.data_.val = WG14_SIGNALS_NULLPTR;
      ret.data_.idx = (size_t) -1;
      return ret;
    }
    assert(map->arr[idx] == WG14_SIGNALS_NULLPTR);
    map->arr[idx] = val;
    ret.data_.val = map->arr[idx];
    ret.data_.idx = idx;
    return ret;
  }
  static inline void WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)(
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t) * map,
  WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr) it)
  {
    assert(it.data_.idx < NSIG);
    map->arr[it.data_.idx] = WG14_SIGNALS_NULLPTR;
  }
#else
#define NAME WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t)
#define KEY_TY int
#define VAL_TY struct WG14_SIGNALS_PREFIX(sighandler_info) *
#define HASH_FN vt_hash_integer
#define CMPR_FN vt_cmpr_integer
#include "verstable.h"
#undef CMPR_FN
#undef HASH_FN
#undef VAL_TY
#undef KEY_TY
#undef NAME

#define signo_to_sighandler_map_t_value(x) (x).data->val
#endif

  /**********************************************************************************/

  struct WG14_SIGNALS_PREFIX(global_signal_decider_t)
  {
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) * prev, *next;
    int refcount;

    WG14_SIGNALS_PREFIX(sig_decide_t) * decider;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value;
  };

  struct WG14_SIGNALS_PREFIX(sighandler_info)
  {
    // Number of active siginstall() holders of this handler: the container is
    // removed from the map when this reaches zero.
    int install_count;
    // Number of references keeping the container alive: the map holds one,
    // and each in-flight raise takes its own so that a concurrent siguninstall
    // cannot free the container while the raise is unlocked inside a decider
    // call (analysis.md 2.2/W4).
    int lifetime_refcount;
#ifndef _WIN32
    struct sigaction old_handler;
#endif
    struct
    {
      struct WG14_SIGNALS_PREFIX(global_signal_decider_t) * front, *back;
    } global_handler;
    struct
    {
      struct WG14_SIGNALS_PREFIX(global_signal_decider_t) * front, *back;
    } deferred_frees;
  };

  // Drop one reference on a sighandler_info container; when the last reference
  // goes away, retire any deciders already moved to deferred_frees, detach any
  // decider still registered for the signal, and free the container. The
  // caller must hold state->lock.
  static void WG14_SIGNALS_PREFIX(sighandler_info_release)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item)
  {
    if(0 == --item->lifetime_refcount)
    {
      while(item->deferred_frees.front != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
        item->deferred_frees.front;
        LIST_REMOVE(item->deferred_frees, i);
        WG14_SIGNALS_FREE(i);
      }
      // Any decider node still registered for this signal (its
      // signal_decider_destroy() handle not yet called) is detached rather than
      // left with prev/next pointing into the freed container: the node stays
      // owned by the handle and is freed when that handle is destroyed
      // (analysis.md 2.23/AA1).
      while(item->global_handler.front != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
        item->global_handler.front;
        LIST_REMOVE(item->global_handler, i);
      }
      WG14_SIGNALS_FREE(item);
    }
  }

  // Returns true if node is currently linked in item->global_handler. The
  // caller must hold state->lock. A node orphaned by a siguninstall that freed
  // its original container is detached, so it is not found here.
  static bool WG14_SIGNALS_PREFIX(sighandler_info_has_decider)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item,
  struct WG14_SIGNALS_PREFIX(global_signal_decider_t) * node)
  {
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *current =
    item->global_handler.front;
    while(current != WG14_SIGNALS_NULLPTR)
    {
      if(current == node)
      {
        return true;
      }
      current = current->next;
    }
    return false;
  }

  struct WG14_SIGNALS_PREFIX(sig_global_state_t)
  {
    WG14_SIGNALS_ATOMIC_PREFIX atomic_uint lock;
    int sighandlers_count;
#ifdef _WIN32
    PVOID vectored_continue_handler;
    LPTOP_LEVEL_EXCEPTION_FILTER old_unhandled_exception_filter;
#endif
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t) signo_to_sighandler_map;
  };
  WG14_SIGNALS_EXTERN struct WG14_SIGNALS_PREFIX(sig_global_state_t) *
  WG14_SIGNALS_PREFIX(sig_global_state)(void)
  {
    static struct WG14_SIGNALS_PREFIX(sig_global_state_t) v;
#if NSIG >= 1024
    // The verstable-variant signo_to_sighandler_map_t is not self-initialising:
    // a zero-initialised table has metadata == NULL, which _get/_insert
    // dereference, crashing every map-touching library operation on an NSIG
    // >= 1024 platform (analysis.md 2.21/Z1). Initialise it exactly once: the
    // original check-and-write ran with no lock and no atomics, a C11 data race
    // between two threads' first concurrent calls (analysis GLIN). The gate
    // below is an atomic single-writer init: the winner's _init() writes are
    // published by its release store below, and every loser spins on an acquire
    // load until the table is visible, so the non-atomic table fields never
    // race. The fast path is a lock-free acquire load.
    static WG14_SIGNALS_ATOMIC_PREFIX atomic_uint map_inited;
    if(!atomic_load_explicit(&map_inited,
                             WG14_SIGNALS_ATOMIC_PREFIX memory_order_acquire))
    {
      unsigned expected = 0;
      if(!atomic_compare_exchange_strong_explicit(
         &map_inited, &expected, 1,
         WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel,
         WG14_SIGNALS_ATOMIC_PREFIX memory_order_acquire))
      {
        // Another thread is initialising the table: wait for it to publish.
        while(atomic_load_explicit(
              &map_inited, WG14_SIGNALS_ATOMIC_PREFIX memory_order_acquire) !=
              2)
        {
          /* spin */;
        }
      }
      else
      {
        WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_init)(
        &v.signo_to_sighandler_map);
        atomic_store_explicit(&map_inited, 2,
                              WG14_SIGNALS_ATOMIC_PREFIX memory_order_release);
      }
    }
#endif
    return &v;
  }


  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) * prev;
#ifndef _WIN32
    const sigset_t *guarded;
    WG14_SIGNALS_PREFIX(sig_recover_t) * recovery;
    WG14_SIGNALS_PREFIX(sig_decide_t) * decider;
    struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
#endif
    jmp_buf buf;
  };
  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) * front;
#ifdef _WIN32
    // Used to detect when stdc_raise() initiated an exception raise
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) *
    stdc_raise_initiated_exception;
#endif
  };
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
  WG14_SIGNALS_EXTERN struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)(void)
  {
    static WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL struct WG14_SIGNALS_PREFIX(
    sig_global_state_tss_state_t) *
    v;
    return &v;
  }
  static int WG14_SIGNALS_PREFIX(sig_global_tss_state_create)(void)
  {
    return 0;
  }
  static int WG14_SIGNALS_PREFIX(sig_global_tss_state_init)(void)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) **state =
    WG14_SIGNALS_PREFIX(sig_tss_state_raw)();
    if(*state != WG14_SIGNALS_NULLPTR)
    {
      return 0;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *mem =
    (struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *)
    WG14_SIGNALS_CALLOC(
    1, sizeof(struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t)));
    if(mem == WG14_SIGNALS_NULLPTR)
    {
      // WG14_SIGNALS_CALLOC() usually sets ENOMEM itself, but set it explicitly
      // so the failure is always observable via errno by stdc_raise()'s callers
      // (plans/analysis.md 3.7).
      errno = ENOMEM;
      return -1;
    }
    *state = mem;
    return WG14_SIGNALS_PREFIX(thread_atexit)(free, mem);
  }
  static struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *
  WG14_SIGNALS_PREFIX(sig_global_tss_state)(void)
  {
    return *WG14_SIGNALS_PREFIX(sig_tss_state_raw)();
  }
  static int WG14_SIGNALS_PREFIX(sig_global_tss_state_destroy)(void)
  {
    return 0;
  }
#else
WG14_SIGNALS_EXTERN WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) *
WG14_SIGNALS_PREFIX(sig_tss_state_raw)(void)
{
  static WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) v;
  return &v;
}
static int sig_global_state_tss_state_create(void **dest)
{
  assert(*dest == WG14_SIGNALS_NULLPTR);
  *dest = WG14_SIGNALS_CALLOC(
  1, sizeof(struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t)));
  return (*dest != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}
static int sig_global_state_tss_state_destroy(void *p)
{
  WG14_SIGNALS_FREE(p);
  return 0;
}
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_create)(void)
{
  if(*WG14_SIGNALS_PREFIX(sig_tss_state_raw)() != WG14_SIGNALS_NULLPTR)
  {
    // A TSS already exists -- e.g. recreated by the documented post-uninstall
    // setup call stdc_raise(0, ...), or by an earlier siginstall whose full
    // siguninstall has not yet run. Reuse it rather than overwrite the slot
    // and leak the old TSS together with every thread registration in it. The
    // async-safe TLS path's create is a no-op and initialises lazily, so this
    // matches that path's semantics (analysis.md Z3 family).
    return 0;
  }
  // Positional, not designated, initialisation: the .ipp is also compiled as
  // C++11 by the header-only C++ test, where designated initialisers are a
  // C++20 extension and would trip -Wc++20-designator under -Werror. The
  // struct has exactly the two members create/destroy in this order.
  const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr) tss_attr = {
  sig_global_state_tss_state_create, sig_global_state_tss_state_destroy};
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
  WG14_SIGNALS_PREFIX(sig_tss_state_raw)(), &tss_attr);
}
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_init)(void)
{
  if(*WG14_SIGNALS_PREFIX(sig_tss_state_raw)() == WG14_SIGNALS_NULLPTR)
  {
    if(-1 == WG14_SIGNALS_PREFIX(sig_global_tss_state_create)())
    {
      return -1;
    }
  }
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
}
static struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *
WG14_SIGNALS_PREFIX(sig_global_tss_state)(void)
{
  return (struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *)
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
}
static int WG14_SIGNALS_PREFIX(sig_global_tss_state_destroy)(void)
{
  // The reset of the static TSS slot was previously dead code after a return,
  // leaving *sig_tss_state_raw() dangling at the freed tss_async_signal_safe_t;
  // any later sigguarded()/stdc_raise() then thread_init'd the freed handle
  // (analysis.md 2.4/Z3). Reset the slot so the next entry recreates the TSS.
  const int ret = WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)());
  *WG14_SIGNALS_PREFIX(sig_tss_state_raw)() = WG14_SIGNALS_NULLPTR;
  return ret;
}
#endif


  static bool WG14_SIGNALS_PREFIX(install_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo);
  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo);

  static bool WG14_SIGNALS_PREFIX(install_sighandler)(const int signo)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    LOCK(state->lock);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
    it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
    &state->signo_to_sighandler_map, signo);
    if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
    {
      struct WG14_SIGNALS_PREFIX(sighandler_info) *newitem =
      (struct WG14_SIGNALS_PREFIX(sighandler_info) *) WG14_SIGNALS_CALLOC(
      1, sizeof(struct WG14_SIGNALS_PREFIX(sighandler_info)));
      if(newitem == WG14_SIGNALS_NULLPTR)
      {
        UNLOCK(state->lock);
        return false;
      }
      newitem->lifetime_refcount = 1;
      if(!WG14_SIGNALS_PREFIX(install_sighandler_impl)(newitem, signo))
      {
        // Release the lock before returning failure: a leaked lock would make
        // every subsequent library call (and any signal delivery through
        // raw_signal_handler) spin forever (analysis.md 2.20/Y1).
        int errcode = errno;
        WG14_SIGNALS_FREE(newitem);
        errno = errcode;
        UNLOCK(state->lock);
        return false;
      }
      it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_insert)(
      &state->signo_to_sighandler_map, signo, newitem);
    }
    signo_to_sighandler_map_t_value(it)->install_count++;
    if(0 == state->sighandlers_count++)
    {
      if(-1 == WG14_SIGNALS_PREFIX(sig_global_tss_state_create)())
      {
        // Roll back the install just committed: a handler left installed with
        // no TSS could never be uninstalled (siginstall has no handle to hand
        // back), and sighandlers_count must not count a TSS that was never
        // created (analysis.md 2.3).
        state->sighandlers_count--;
        if(0 == --signo_to_sighandler_map_t_value(it)->install_count)
        {
          struct WG14_SIGNALS_PREFIX(sighandler_info) *item =
          signo_to_sighandler_map_t_value(it);
          (void) WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(item, signo);
          WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)
          (&state->signo_to_sighandler_map, it);
          WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
        }
        UNLOCK(state->lock);
        return false;
      }
    }
    UNLOCK(state->lock);
    return true;
  }

  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler)(const int signo)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    LOCK(state->lock);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
    it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
    &state->signo_to_sighandler_map, signo);
    if(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
    {
      while(signo_to_sighandler_map_t_value(it)->deferred_frees.front !=
            WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
        signo_to_sighandler_map_t_value(it)->deferred_frees.front;
        LIST_REMOVE(signo_to_sighandler_map_t_value(it)->deferred_frees, i);
        WG14_SIGNALS_FREE(i);
      }
      const bool need_to_destroy_tss = (0 == --state->sighandlers_count);
      if(0 == --signo_to_sighandler_map_t_value(it)->install_count)
      {
        struct WG14_SIGNALS_PREFIX(sighandler_info) *item =
        signo_to_sighandler_map_t_value(it);
        (void) WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(item, signo);
        WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)
        (&state->signo_to_sighandler_map, it);
        // Drop the map's reference on the container; an in-flight raise holds
        // its own reference and frees the container when it releases
        // (analysis.md 2.2/W4).
        WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
      }
      if(need_to_destroy_tss)
      {
        (void) WG14_SIGNALS_PREFIX(sig_global_tss_state_destroy)();
      }
    }
    UNLOCK(state->lock);
    return true;
  }

  void *WG14_SIGNALS_PREFIX(siginstall)(const sigset_t *guarded)
  {
    sigset_t *ret = (sigset_t *) WG14_SIGNALS_MALLOC(sizeof(sigset_t));
    if(ret == WG14_SIGNALS_NULLPTR)
    {
      return WG14_SIGNALS_NULLPTR;
    }
    if(guarded == WG14_SIGNALS_NULLPTR)
    {
      // "All the standard POSIX signals" (the documented contract for a null
      // guarded set) is every signal below NSIG except the libc-internal and
      // realtime ones: a raw WG14_SIGNALS_SIGFILLSET() on glibc would also
      // cover SIGCANCEL/SIGSETXID (glibc's internal pthread-cancellation/setxid
      // signals) and the realtime range, which must not have library handlers
      // installed (analysis.md RTIM). Fill first, then exclude those ranges,
      // so the semantics stay "all signals minus the internal/realtime ones"
      // rather than "whatever the helper sets happen to contain".
      WG14_SIGNALS_SIGFILLSET(ret);
      // The realtime range. SIGRTMIN/SIGRTMAX may be compile-time constants
      // (musl, BSD) or, on glibc, the runtime functions
      // __libc_current_sigrtmin()/__libc_current_sigrtmax() -- both forms are
      // fine to evaluate here because siginstall() is not async-signal-safe.
#if defined(SIGRTMIN) && defined(SIGRTMAX)
      for(int signo = (int) SIGRTMIN; signo <= (int) SIGRTMAX; signo++)
      {
        WG14_SIGNALS_SIGDELSET(ret, signo);
      }
#endif
    }
    else
    {
      *ret = *guarded;
    }
    // libc-internal signals are never installable, regardless of which guarded
    // set was supplied: SIGCANCEL/SIGSETXID are glibc's own
    // pthread-cancellation/setxid signals and must never be intercepted by a
    // library handler (analysis.md RTIM). This applies to explicit guarded
    // inputs too -- a caller doing WG14_SIGNALS_SIGFILLSET(&set);
    // siginstall(&set) would otherwise hit them through the same handle.
#ifdef SIGCANCEL
    WG14_SIGNALS_SIGDELSET(ret, SIGCANCEL);
#endif
#ifdef SIGSETXID
    WG14_SIGNALS_SIGDELSET(ret, SIGSETXID);
#endif
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
#ifdef __FILC__
      if(zis_unsafe_signal_for_handlers(signo))
      {
        continue;
      }
#endif
      if(WG14_SIGNALS_SIGISMEMBER(ret, signo))
      {
        if(!WG14_SIGNALS_PREFIX(install_sighandler)(signo))
        {
          // Roll back the signals this call already installed: a partial
          // install that returned NULL would otherwise leave handlers
          // installed with no handle to uninstall them, and a subsequent
          // siginstall would double-count them (analysis.md 3.8). Every signal
          // processed before this failure was successfully installed (the loop
          // returns at the first failure), so uninstalling exactly the members
          // of `ret` below this signo undoes this call's contribution.
          // uninstall_sighandler() always succeeds and decrements the signal's
          // install_count, fully uninstalling only when it reaches zero, so a
          // signal that was already installed by an earlier siginstall stays
          // installed at its previous reference count.
          const int errcode = errno;
          for(int rollback_signo = 1; rollback_signo < signo; rollback_signo++)
          {
            if(rollback_signo == SIGKILL || rollback_signo == SIGSTOP)
            {
              continue;
            }
#ifdef __FILC__
            if(zis_unsafe_signal_for_handlers(rollback_signo))
            {
              continue;
            }
#endif
            if(WG14_SIGNALS_SIGISMEMBER(ret, rollback_signo))
            {
              (void) WG14_SIGNALS_PREFIX(uninstall_sighandler)(rollback_signo);
            }
          }
          WG14_SIGNALS_FREE(ret);
          errno = errcode;
          return WG14_SIGNALS_NULLPTR;
        }
      }
    }
    return ret;
  }

  int WG14_SIGNALS_PREFIX(siguninstall)(void *ss)
  {
    if(ss == WG14_SIGNALS_NULLPTR)
    {
      errno = EINVAL;
      return -1;
    }
    sigset_t *sigset = (sigset_t *) ss;
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
      if(WG14_SIGNALS_SIGISMEMBER(sigset, signo))
      {
        if(!WG14_SIGNALS_PREFIX(uninstall_sighandler)(signo))
        {
          return -1;
        }
      }
    }
    WG14_SIGNALS_FREE(ss);
    return 0;
  }

  int WG14_SIGNALS_PREFIX(siguninstall_system)(int version)
  {
    if(version != 0)
    {
      errno = EINVAL;
      return -1;
    }
    return 0;
  }

  void *WG14_SIGNALS_PREFIX(signal_decider_create)(
  const sigset_t *guarded, bool callfirst,
  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
  {
    if(guarded == WG14_SIGNALS_NULLPTR)
    {
      errno = EINVAL;
      return WG14_SIGNALS_NULLPTR;
    }
    size_t signo_count = 0;
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
      if(WG14_SIGNALS_SIGISMEMBER(guarded, signo))
      {
        signo_count++;
      }
    }
    if(signo_count == 0 || decider == WG14_SIGNALS_NULLPTR)
    {
      errno = EINVAL;
      return WG14_SIGNALS_NULLPTR;
    }
    const size_t sigset_t_size =
    (sizeof(sigset_t) +
     sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *) - 1) &
    ~(sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *) - 1);
    void *ret = WG14_SIGNALS_MALLOC(
    sigset_t_size + (signo_count + 1) * sizeof(struct WG14_SIGNALS_PREFIX(
                                        global_signal_decider_t) *));
    if(ret == WG14_SIGNALS_NULLPTR)
    {
      return WG14_SIGNALS_NULLPTR;
    }
    WG14_SIGNALS_MEMSET(ret, 0,
                        sigset_t_size +
                        (signo_count + 1) * sizeof(struct WG14_SIGNALS_PREFIX(
                                            global_signal_decider_t) *));
    *(sigset_t *) ret = *guarded;
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **retp =
    (struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **) ((char *) ret +
                                                              sigset_t_size);

    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
      if(WG14_SIGNALS_SIGISMEMBER(guarded, signo))
      {
        LOCK(state->lock);
        WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
        it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
        &state->signo_to_sighandler_map, signo);
        if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
        {
          // We don't have a handler installed for that signal: record the NULL
          // slot and report the warning after releasing the lock --
          // WG14_SIGNALS_STDERR_PRINTF is slow and can itself trigger a signal
          // delivery while state->lock is held (plans/analysis.md SDCF, SPIN).
          *retp++ = WG14_SIGNALS_NULLPTR;
          UNLOCK(state->lock);
          WG14_SIGNALS_STDERR_PRINTF(
          "WARNING: signal_decider_create() installing decider for signal %d "
          "but "
          "handler was never installed for that signal.\n",
          signo);
          continue;
        }
        struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
        (struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *)
        WG14_SIGNALS_CALLOC(
        1, sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t)));
        if(i == WG14_SIGNALS_NULLPTR)
        {
          int errcode = errno;
          UNLOCK(state->lock);
          WG14_SIGNALS_PREFIX(signal_decider_destroy)(ret);
          errno = errcode;
          return WG14_SIGNALS_NULLPTR;
        }
        i->refcount = 1;
        i->decider = decider;
        i->value = value;
        if(callfirst)
        {
          LIST_INSERT_FRONT(signo_to_sighandler_map_t_value(it)->global_handler,
                            i);
        }
        else
        {
          LIST_INSERT_BACK(signo_to_sighandler_map_t_value(it)->global_handler,
                           i);
        }
        *retp++ = i;
        UNLOCK(state->lock);
      }
    }
    return ret;
  }

  int WG14_SIGNALS_PREFIX(signal_decider_destroy)(void *p)
  {
    if(p == WG14_SIGNALS_NULLPTR)
    {
      errno = EINVAL;
      return -1;
    }
    // The handle is always recognised (a non-NULL opaque pointer) and is
    // always freed below, so the destroy succeeds -- 0 per the N3924 7.14.2.8
    // return contract ("If successful, this function returns zero"). The
    // former -1-on-no-matching-slots was a misleading error signal: it fired
    // exactly when the guarded signals have no live decider nodes (e.g. a
    // partially built handle from signal_decider_create()'s failure path whose
    // slots are all NULL) even though the handle was removed
    // (plans/analysis.md SDCF).
    int ret = 0;
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    const sigset_t *guarded = (const sigset_t *) p;
    const size_t sigset_t_size =
    (sizeof(sigset_t) +
     sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *) - 1) &
    ~(sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *) - 1);
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **retp =
    (struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **) ((char *) p +
                                                              sigset_t_size);
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
      LOCK(state->lock);
      WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
      it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
      &state->signo_to_sighandler_map, signo);
      if(!WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
      {
        while(signo_to_sighandler_map_t_value(it)->deferred_frees.front !=
              WG14_SIGNALS_NULLPTR)
        {
          struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
          signo_to_sighandler_map_t_value(it)->deferred_frees.front;
          LIST_REMOVE(signo_to_sighandler_map_t_value(it)->deferred_frees, i);
          WG14_SIGNALS_FREE(i);
        }
        if(WG14_SIGNALS_SIGISMEMBER(guarded, signo))
        {
          if(*retp != WG14_SIGNALS_NULLPTR)
          {
            if(0 == --(*retp)->refcount)
            {
              // The node may have been orphaned (detached) by a siguninstall
              // that freed its original container, leaving this signal's new
              // container without it; only LIST_REMOVE when the node is still
              // linked in the current container (analysis.md 2.23/AA1).
              if(WG14_SIGNALS_PREFIX(sighandler_info_has_decider)(
                 signo_to_sighandler_map_t_value(it), *retp))
              {
                LIST_REMOVE(signo_to_sighandler_map_t_value(it)->global_handler,
                            *retp);
              }
              else
              {
                (*retp)->next = (*retp)->prev = WG14_SIGNALS_NULLPTR;
              }
              // The node is no longer referenced by any container or in-flight
              // raise (the map-entry branch is only reached when no raise holds
              // it, since the raise path bumps refcount under the same lock
              // before its unlocked decider call), so free it now. Leaving it
              // to the post-unlock block would decrement the refcount a second
              // time 0 -> -1 and never free it (analysis.md 2.24/AB1).
              WG14_SIGNALS_FREE(*retp);
              *retp = WG14_SIGNALS_NULLPTR;
            }
            else
            {
              // He will be freed when the handler exits
              *retp = WG14_SIGNALS_NULLPTR;
            }
          }
        }
      }
      UNLOCK(state->lock);
      if(WG14_SIGNALS_SIGISMEMBER(guarded, signo))
      {
        if(*retp != WG14_SIGNALS_NULLPTR)
        {
          // The signal is not currently installed (its container was released
          // by a siguninstall). The node may still be pinned by an in-flight
          // raise, so only free it once its refcount reaches zero; a pinned
          // node is retired via deferred_frees when the raise completes
          // (analysis.md 2.23/AA1 concurrent sibling).
          if(0 == --(*retp)->refcount)
          {
            WG14_SIGNALS_FREE(*retp);
          }
        }
        retp++;
      }
    }
    WG14_SIGNALS_FREE(p);
    return ret;
  }

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef __cplusplus
}
#endif

#endif
