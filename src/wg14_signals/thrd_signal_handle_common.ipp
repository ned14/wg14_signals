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

#include "wg14_signals/thrd_signal_handle.h"

#include "linked_list.h"
#include "lock_unlock.h"
#include "wg14_signals/tss_async_signal_safe.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct sighandler_t;

#if NSIG < 1024
typedef struct signo_to_sighandler_map_t
{
  struct sighandler_t *arr[NSIG];
} signo_to_sighandler_map_t;

struct signo_to_sighandler_map_t_itr_data
{
  struct sighandler_t *val;
  size_t idx;
};
typedef struct signo_to_sighandler_map_t_itr
{
  struct signo_to_sighandler_map_t_itr_data data_, *data;
} signo_to_sighandler_map_t_itr;

static inline signo_to_sighandler_map_t_itr
signo_to_sighandler_map_t_get(signo_to_sighandler_map_t *map, int idx)
{
  signo_to_sighandler_map_t_itr ret;
  if(idx < 0 || idx >= NSIG || map->arr[idx] == WG14_SIGNALS_NULLPTR)
  {
    ret.data_.val = WG14_SIGNALS_NULLPTR;
    ret.data_.idx = (size_t) -1;
    ret.data = WG14_SIGNALS_NULLPTR;
    return ret;
  }
  ret.data_.val = map->arr[idx];
  ret.data_.idx = idx;
  ret.data = &ret.data_;
  return ret;
}
static inline bool
signo_to_sighandler_map_t_is_end(signo_to_sighandler_map_t_itr it)
{
  return it.data == WG14_SIGNALS_NULLPTR;
}
static inline signo_to_sighandler_map_t_itr
signo_to_sighandler_map_t_insert(signo_to_sighandler_map_t *map, int idx,
                                 struct sighandler_t *val)
{
  signo_to_sighandler_map_t_itr ret;
  if(idx < 0 || idx >= NSIG)
  {
    ret.data_.val = WG14_SIGNALS_NULLPTR;
    ret.data_.idx = (size_t) -1;
    ret.data = WG14_SIGNALS_NULLPTR;
    return ret;
  }
  assert(map->arr[idx] == WG14_SIGNALS_NULLPTR);
  map->arr[idx] = val;
  ret.data_.val = map->arr[idx];
  ret.data_.idx = idx;
  ret.data = &ret.data_;
  return ret;
}
static inline void
signo_to_sighandler_map_t_erase_itr(signo_to_sighandler_map_t *map,
                                    signo_to_sighandler_map_t_itr it)
{
  assert(it.data->idx < NSIG);
  map->arr[it.data->idx] = WG14_SIGNALS_NULLPTR;
}
#else
#define NAME signo_to_sighandler_map_t
#define KEY_TY int
#define VAL_TY struct sighandler_t *
#include "verstable.h"
#endif

/**********************************************************************************/

struct global_signal_decider_t
{
  struct global_signal_decider_t *prev, *next;

  WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider;
  union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value;
};

struct sighandler_t
{
  int count;
#ifndef _WIN32
  struct sigaction old_handler;
#endif
  struct
  {
    struct global_signal_decider_t *front, *back;
  } global_handler;
};

struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t)
{
  atomic_uint lock;
  int sighandlers_count;
#ifdef _WIN32
  PVOID vectored_continue_handler;
  LPTOP_LEVEL_EXCEPTION_FILTER old_unhandled_exception_filter;
#endif
  signo_to_sighandler_map_t signo_to_sighandler_map;
  WG14_SIGNALS_PREFIX(tss_async_signal_safe) tss_state;
};
WG14_SIGNALS_EXTERN struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *
WG14_SIGNALS_PREFIX(thrd_signal_global_state)(void)
{
  static struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) v;
  return &v;
}

struct thrd_signal_global_state_tss_state_per_frame_t
{
  struct thrd_signal_global_state_tss_state_per_frame_t *prev;
#ifndef _WIN32
  const sigset_t *guarded;
  WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider;
  struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info) rsi;
#endif
  jmp_buf buf;
};
struct thrd_signal_global_state_tss_state_t
{
  struct thrd_signal_global_state_tss_state_per_frame_t *front;
};
static int thrd_signal_global_state_tss_state_create(void **dest)
{
  assert(*dest == WG14_SIGNALS_NULLPTR);
  *dest = calloc(1, sizeof(struct thrd_signal_global_state_tss_state_t));
  return (*dest != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}
static int thrd_signal_global_state_tss_state_destroy(void *p)
{
  free(p);
  return 0;
}
static int thrd_signal_global_tss_state_init(void)
{
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
  return WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  state->tss_state);
}
static struct thrd_signal_global_state_tss_state_t *
thrd_signal_global_tss_state(void)
{
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
  return (struct thrd_signal_global_state_tss_state_t *) WG14_SIGNALS_PREFIX(
  tss_async_signal_safe_get)(state->tss_state);
}

static bool install_sighandler_impl(struct sighandler_t *item, const int signo);
static bool install_sighandler(const int signo)
{
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
  LOCK(state->lock);
  signo_to_sighandler_map_t_itr it =
  signo_to_sighandler_map_t_get(&state->signo_to_sighandler_map, signo);
  if(signo_to_sighandler_map_t_is_end(it))
  {
    struct sighandler_t *newitem =
    (struct sighandler_t *) calloc(1, sizeof(struct sighandler_t));
    if(newitem == WG14_SIGNALS_NULLPTR)
    {
      UNLOCK(state->lock);
      return false;
    }
    if(!install_sighandler_impl(newitem, signo))
    {
      int errcode = errno;
      free(newitem);
      errno = errcode;
      return false;
    }
    it = signo_to_sighandler_map_t_insert(&state->signo_to_sighandler_map,
                                          signo, newitem);
  }
  it.data->val->count++;
  if(0 == state->sighandlers_count++)
  {
    const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
    tss_attr = {.create = thrd_signal_global_state_tss_state_create,
                .destroy = thrd_signal_global_state_tss_state_destroy};
    if(-1 == WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
             &state->tss_state, &tss_attr))
    {
      UNLOCK(state->lock);
      return false;
    }
  }
  UNLOCK(state->lock);
  return true;
}

static bool uninstall_sighandler_impl(struct sighandler_t *item,
                                      const int signo);
static bool uninstall_sighandler(const int signo)
{
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
  LOCK(state->lock);
  signo_to_sighandler_map_t_itr it =
  signo_to_sighandler_map_t_get(&state->signo_to_sighandler_map, signo);
  if(!signo_to_sighandler_map_t_is_end(it))
  {
    const bool need_to_destroy_tss = (0 == --state->sighandlers_count);
    if(0 == --it.data->val->count)
    {
      (void) uninstall_sighandler_impl(it.data->val, signo);
      free(it.data->val);
      signo_to_sighandler_map_t_erase_itr(&state->signo_to_sighandler_map, it);
    }
    if(need_to_destroy_tss)
    {
      (void) WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
      state->tss_state);
    }
  }
  UNLOCK(state->lock);
  return true;
}

void *WG14_SIGNALS_PREFIX(modern_signals_install)(const sigset_t *guarded,
                                                  int version)
{
  if(version != 0)
  {
    errno = EINVAL;
    return WG14_SIGNALS_NULLPTR;
  }
  sigset_t *ret = malloc(sizeof(sigset_t));
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
    if(sigismember(ret, signo))
    {
      if(!install_sighandler(signo))
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

bool WG14_SIGNALS_PREFIX(modern_signals_uninstall)(void *ss)
{
  sigset_t *sigset = (sigset_t *) ss;
  for(int signo = 1; signo < NSIG; signo++)
  {
    if(signo == SIGKILL || signo == SIGSTOP)
    {
      continue;
    }
    if(sigismember(sigset, signo))
    {
      if(!uninstall_sighandler(signo))
      {
        return false;
      }
    }
  }
  return true;
}

bool WG14_SIGNALS_PREFIX(modern_signals_uninstall_system)(int version)
{
  if(version != 0)
  {
    errno = EINVAL;
    return false;
  }
  return true;
}

void *WG14_SIGNALS_PREFIX(signal_decider_create)(
const sigset_t *guarded, bool callfirst,
WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider,
union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value)
{
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
  void *ret =
  malloc(sizeof(sigset_t) +
         (signo_count + 1) * sizeof(struct global_signal_decider_t *));
  if(ret == WG14_SIGNALS_NULLPTR)
  {
    return WG14_SIGNALS_NULLPTR;
  }
  memset(ret, 0,
         sizeof(sigset_t) +
         (signo_count + 1) * sizeof(struct global_signal_decider_t *));
  *(sigset_t *) ret = *guarded;
  struct global_signal_decider_t **retp =
  (struct global_signal_decider_t **) ((char *) ret + sizeof(sigset_t));

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
      signo_to_sighandler_map_t_itr it =
      signo_to_sighandler_map_t_get(&state->signo_to_sighandler_map, signo);
      if(signo_to_sighandler_map_t_is_end(it))
      {
        // We don't have a handler installed for that signal
        WG14_SIGNALS_STDERR_PRINTF(
        "WARNING: signal_decider_create() installing decider for signal %d but "
        "handler was never installed for that signal.\n",
        signo);
        continue;
      }
      struct global_signal_decider_t *i =
      (struct global_signal_decider_t *) calloc(
      1, sizeof(struct global_signal_decider_t));
      if(i == WG14_SIGNALS_NULLPTR)
      {
        int errcode = errno;
        UNLOCK(state->lock);
        WG14_SIGNALS_PREFIX(signal_decider_destroy)(ret);
        errno = errcode;
        return WG14_SIGNALS_NULLPTR;
      }
      i->decider = decider;
      i->value = value;
      if(callfirst)
      {
        LIST_INSERT_FRONT(it.data->val->global_handler, i);
      }
      else
      {
        LIST_INSERT_BACK(it.data->val->global_handler, i);
      }
      *retp++ = i;
    }
    UNLOCK(state->lock);
  }
  return ret;
}

bool WG14_SIGNALS_PREFIX(signal_decider_destroy)(void *p)
{
  bool ret = false;
  struct WG14_SIGNALS_PREFIX(thrd_signal_global_state_t) *state =
  WG14_SIGNALS_PREFIX(thrd_signal_global_state)();
  const sigset_t *guarded = (const sigset_t *) p;
  struct global_signal_decider_t **retp =
  (struct global_signal_decider_t **) ((char *) p + sizeof(sigset_t));
  for(int signo = 1; signo < NSIG; signo++)
  {
    if(signo == SIGKILL || signo == SIGSTOP)
    {
      continue;
    }
    if(sigismember(guarded, signo))
    {
      LOCK(state->lock);
      signo_to_sighandler_map_t_itr it =
      signo_to_sighandler_map_t_get(&state->signo_to_sighandler_map, signo);
      if(!signo_to_sighandler_map_t_is_end(it))
      {
        if(*retp != WG14_SIGNALS_NULLPTR)
        {
          LIST_REMOVE(it.data->val->global_handler, *retp);
          ret = true;
        }
      }
      if(*retp != WG14_SIGNALS_NULLPTR)
      {
        free(*retp);
      }
      retp++;
      UNLOCK(state->lock);
    }
  }
  free(p);
  return ret;
}
