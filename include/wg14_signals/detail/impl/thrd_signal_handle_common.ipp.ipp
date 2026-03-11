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

    WG14_SIGNALS_PREFIX(thrd_signal_decide_t) * decider;
    union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value;
  };

  struct WG14_SIGNALS_PREFIX(sighandler_info)
  {
    int count;
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

  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t)
  {
    WG14_SIGNALS_ATOMIC_PREFIX atomic_uint lock;
    int sighandlers_count;
#ifdef _WIN32
    PVOID vectored_continue_handler;
    LPTOP_LEVEL_EXCEPTION_FILTER old_unhandled_exception_filter;
#endif
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t) signo_to_sighandler_map;
  };
  WG14_SIGNALS_EXTERN struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)(void)
  {
    static struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) v;
    return &v;
  }


  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_per_frame_t)
  {
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_per_frame_t) *
    prev;
#ifndef _WIN32
    const sigset_t *guarded;
    WG14_SIGNALS_PREFIX(thrd_signal_recover_t) * recovery;
    WG14_SIGNALS_PREFIX(thrd_signal_decide_t) * decider;
    struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info) rsi;
#endif
    jmp_buf buf;
  };
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t)
  {
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_per_frame_t) *
    front;
  };
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
  WG14_SIGNALS_EXTERN struct WG14_SIGNALS_PREFIX(
  thrd_signal_global_state_tss_state_t) *
  *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)(void)
  {
    static WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL struct WG14_SIGNALS_PREFIX(
    thrd_signal_global_state_tss_state_t) *
    v;
    return &v;
  }
  static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_create)(void)
  {
    return 0;
  }
  static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_init)(void)
  {
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) **state =
    WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)();
    if(*state != WG14_SIGNALS_NULLPTR)
    {
      return 0;
    }
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) *mem =
    (struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) *) calloc(
    1,
    sizeof(struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t)));
    if(mem == WG14_SIGNALS_NULLPTR)
    {
      return -1;
    }
    *state = mem;
    return WG14_SIGNALS_PREFIX(thread_atexit)(free, mem);
  }
  static struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) *
  WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state)(void)
  {
    return *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)();
  }
  static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_destroy)(void)
  {
    return 0;
  }
#else
WG14_SIGNALS_EXTERN WG14_SIGNALS_PREFIX(tss_async_signal_safe) *
WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)(void)
{
  static WG14_SIGNALS_PREFIX(tss_async_signal_safe) v;
  return &v;
}
static int thrd_signal_global_state_tss_state_create(void **dest)
{
  assert(*dest == WG14_SIGNALS_NULLPTR);
  *dest = calloc(
  1, sizeof(struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t)));
  return (*dest != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}
static int thrd_signal_global_state_tss_state_destroy(void *p)
{
  free(p);
  return 0;
}
static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_create)(void)
{
  const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
  tss_attr = {.create = thrd_signal_global_state_tss_state_create,
              .destroy = thrd_signal_global_state_tss_state_destroy};
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
  WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)(), &tss_attr);
}
static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_init)(void)
{
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)());
}
static struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) *
WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state)(void)
{
  return (struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_tss_state_t) *)
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(
  *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)());
}
static int WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_destroy)(void)
{
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
  *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)());
  *WG14_SIGNALS_PREFIX(thrd_signal_tss_state_raw)() = WG14_SIGNALS_NULLPTR;
}
#endif


  static bool WG14_SIGNALS_PREFIX(install_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo);
  static bool WG14_SIGNALS_PREFIX(install_sighandler)(const int signo)
  {
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
    WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
    LOCK(state->lock);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
    it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
    &state->signo_to_sighandler_map, signo);
    if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
    {
      struct WG14_SIGNALS_PREFIX(sighandler_info) *newitem =
      (struct WG14_SIGNALS_PREFIX(sighandler_info) *) calloc(
      1, sizeof(struct WG14_SIGNALS_PREFIX(sighandler_info)));
      if(newitem == WG14_SIGNALS_NULLPTR)
      {
        UNLOCK(state->lock);
        return false;
      }
      if(!WG14_SIGNALS_PREFIX(install_sighandler_impl)(newitem, signo))
      {
        int errcode = errno;
        free(newitem);
        errno = errcode;
        return false;
      }
      it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_insert)(
      &state->signo_to_sighandler_map, signo, newitem);
    }
    signo_to_sighandler_map_t_value(it)->count++;
    if(0 == state->sighandlers_count++)
    {
      if(-1 == WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_create)())
      {
        UNLOCK(state->lock);
        return false;
      }
    }
    UNLOCK(state->lock);
    return true;
  }

  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo);
  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler)(const int signo)
  {
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
    WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
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
        free(i);
      }
      const bool need_to_destroy_tss = (0 == --state->sighandlers_count);
      if(0 == --signo_to_sighandler_map_t_value(it)->count)
      {
        (void) WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
        signo_to_sighandler_map_t_value(it), signo);
        free(signo_to_sighandler_map_t_value(it));
        WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_erase_itr)
        (&state->signo_to_sighandler_map, it);
      }
      if(need_to_destroy_tss)
      {
        (void) WG14_SIGNALS_PREFIX(thrd_signal_global_tss_state_destroy)();
      }
    }
    UNLOCK(state->lock);
    return true;
  }

  void *WG14_SIGNALS_PREFIX(threadsafe_signals_install)(const sigset_t *guarded)
  {
    sigset_t *ret = (sigset_t *) malloc(sizeof(sigset_t));
    if(ret == WG14_SIGNALS_NULLPTR)
    {
      return WG14_SIGNALS_NULLPTR;
    }
    if(guarded == WG14_SIGNALS_NULLPTR)
    {
      sigfillset(ret);
    }
    else
    {
      *ret = *guarded;
    }
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
      if(sigismember(ret, signo))
      {
        if(!WG14_SIGNALS_PREFIX(install_sighandler)(signo))
        {
          const int errcode = errno;
          free(ret);
          errno = errcode;
          return WG14_SIGNALS_NULLPTR;
        }
      }
    }
    return ret;
  }

  int WG14_SIGNALS_PREFIX(threadsafe_signals_uninstall)(void *ss)
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
      if(sigismember(sigset, signo))
      {
        if(!WG14_SIGNALS_PREFIX(uninstall_sighandler)(signo))
        {
          return -1;
        }
      }
    }
    free(ss);
    return 0;
  }

  int WG14_SIGNALS_PREFIX(threadsafe_signals_uninstall_system)(int version)
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
  WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider,
  union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value)
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
      if(sigismember(guarded, signo))
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
    void *ret =
    malloc(sigset_t_size +
           (signo_count + 1) *
           sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *));
    if(ret == WG14_SIGNALS_NULLPTR)
    {
      return WG14_SIGNALS_NULLPTR;
    }
    memset(ret, 0,
           sigset_t_size +
           (signo_count + 1) *
           sizeof(struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *));
    *(sigset_t *) ret = *guarded;
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **retp =
    (struct WG14_SIGNALS_PREFIX(global_signal_decider_t) **) ((char *) ret +
                                                              sigset_t_size);

    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
    WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
    for(int signo = 1; signo < NSIG; signo++)
    {
      if(signo == SIGKILL || signo == SIGSTOP)
      {
        continue;
      }
      if(sigismember(guarded, signo))
      {
        LOCK(state->lock);
        WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
        it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
        &state->signo_to_sighandler_map, signo);
        if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
        {
          // We don't have a handler installed for that signal
          WG14_SIGNALS_STDERR_PRINTF(
          "WARNING: signal_decider_create() installing decider for signal %d "
          "but "
          "handler was never installed for that signal.\n",
          signo);
          continue;
        }
        struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *i =
        (struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *) calloc(
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
    int ret = -1;
    struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
    WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
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
          free(i);
        }
        if(sigismember(guarded, signo))
        {
          if(*retp != WG14_SIGNALS_NULLPTR)
          {
            if(0 == --(*retp)->refcount)
            {
              LIST_REMOVE(signo_to_sighandler_map_t_value(it)->global_handler,
                          *retp);
            }
            else
            {
              // He will be freed when the handler exits
              *retp = WG14_SIGNALS_NULLPTR;
            }
            ret = 0;
          }
        }
      }
      UNLOCK(state->lock);
      if(sigismember(guarded, signo))
      {
        if(*retp != WG14_SIGNALS_NULLPTR)
        {
          free(*retp);
        }
        retp++;
      }
    }
    free(p);
    return ret;
  }

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef __cplusplus
}
#endif
