/* Proposed WG14 improved signals support
(C) 2024 - 2026 Niall Douglas <http://www.nedproductions.biz/>
File Created: Aug 2026


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

/* The single implementation of thread_atexit(), usable from C and C++
   translation units and from the library (src/wg14_signals/thread_atexit.c).
   This removes the previous C++ implementation, so the library has no C++
   runtime dependency (analysis.md 5.2, Y7; AGENTS.md rule 4).

   On platforms that supply __cxa_thread_atexit() (the Itanium ABI thread-local
   destructor registration primitive, discovered by a CMake configure probe) it
   is used preferentially and the pthread-key/FLS fallback below is excluded.
   Its return value is deliberately ignored: on some platforms (e.g. macOS) it
   is not reliable even though the registration works, so thread_atexit()
   reports success once the callback has been handed to the runtime.

   Otherwise the callbacks are drained at thread exit via the platform's
   thread-exit hook: pthread_key destructors on POSIX, fiber-local-storage
   callbacks on Windows.

   The list is per-thread and per-TU, so no locking is needed for the list
   itself; the key initialisation is guarded by pthread_once on POSIX.
*/

#ifndef WG14_SIGNALS_THREAD_ATEXIT_C_IPP
#define WG14_SIGNALS_THREAD_ATEXIT_C_IPP

#include "thread_atexit.h"

#include <errno.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT
// __cxa_thread_atexit() (Itanium ABI) registers a callback to run when the
// calling thread exits; the "dso_symbol" argument is any symbol address in the
// DSO that made the registration.
#ifdef __cplusplus
extern "C"
#endif
int __cxa_thread_atexit(void (*func)(void *), void *obj, void *dso_symbol);
static void WG14_SIGNALS_PREFIX(thread_atexit_dso_symbol)(void) {}

//! \brief Register a callback to run when the calling thread exits.
WG14_SIGNALS_EXTERN int
WG14_SIGNALS_PREFIX(thread_atexit)(void (*func)(void *obj), void *obj)
{
  // The return value is not reliable on every platform that supplies the
  // symbol (e.g. macOS returns garbage while the registration works), so it is
  // ignored.
  __cxa_thread_atexit(
  func, obj, (void *) &WG14_SIGNALS_PREFIX(thread_atexit_dso_symbol));
  return 0;
}
#else
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct WG14_SIGNALS_PREFIX(thread_atexit_item_t)
{
  void (*func)(void *obj);
  void *obj;
  struct WG14_SIGNALS_PREFIX(thread_atexit_item_t) * next;
} WG14_SIGNALS_PREFIX(thread_atexit_item_t);

#ifndef _WIN32
static WG14_SIGNALS_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_atexit_item_t) *
WG14_SIGNALS_PREFIX(thread_atexit_items) = WG14_SIGNALS_NULLPTR;
static pthread_key_t WG14_SIGNALS_PREFIX(thread_atexit_key);

static void WG14_SIGNALS_PREFIX(thread_atexit_run)(void *unused)
{
  (void) unused;
  // Prevent pthread from re-invoking the destructor
  // (PTHREAD_DESTRUCTOR_ITERATIONS).
  pthread_setspecific(WG14_SIGNALS_PREFIX(thread_atexit_key),
                      WG14_SIGNALS_NULLPTR);
  WG14_SIGNALS_PREFIX(thread_atexit_item_t) *items =
  WG14_SIGNALS_PREFIX(thread_atexit_items);
  WG14_SIGNALS_PREFIX(thread_atexit_items) = WG14_SIGNALS_NULLPTR;
  while(items != WG14_SIGNALS_NULLPTR)
  {
    WG14_SIGNALS_PREFIX(thread_atexit_item_t) *next = items->next;
    items->func(items->obj);
    free(items);
    items = next;
  }
}

static void WG14_SIGNALS_PREFIX(thread_atexit_key_init)(void)
{
  if(0 != pthread_key_create(&WG14_SIGNALS_PREFIX(thread_atexit_key),
                             WG14_SIGNALS_PREFIX(thread_atexit_run)))
  {
    WG14_SIGNALS_PREFIX(thread_atexit_key) = (pthread_key_t) -1;
  }
}
#else
static void WINAPI WG14_SIGNALS_PREFIX(thread_atexit_fls_callback)(void *pv)
{
  WG14_SIGNALS_PREFIX(thread_atexit_item_t) *items =
  (WG14_SIGNALS_PREFIX(thread_atexit_item_t) *) pv;
  WG14_SIGNALS_PREFIX(thread_atexit_item_t) * next;
  for(; items != WG14_SIGNALS_NULLPTR; items = next)
  {
    next = items->next;
    items->func(items->obj);
    free(items);
  }
}
static DWORD WG14_SIGNALS_PREFIX(thread_atexit_fls_key)(void)
{
  static DWORD key = FLS_OUT_OF_INDEXES;
  DWORD current = key;
  if(current == FLS_OUT_OF_INDEXES)
  {
    current = FlsAlloc(WG14_SIGNALS_PREFIX(thread_atexit_fls_callback));
    (void) InterlockedCompareExchange((volatile LONG *) &key, (LONG) current,
                                      (LONG) FLS_OUT_OF_INDEXES);
  }
  return key;
}
#endif

//! \brief Register a callback to run when the calling thread exits.
WG14_SIGNALS_EXTERN int
WG14_SIGNALS_PREFIX(thread_atexit)(void (*func)(void *obj), void *obj)
{
  WG14_SIGNALS_PREFIX(thread_atexit_item_t) *item =
  (WG14_SIGNALS_PREFIX(thread_atexit_item_t) *) malloc(
  sizeof(WG14_SIGNALS_PREFIX(thread_atexit_item_t)));
  if(item == WG14_SIGNALS_NULLPTR)
  {
    errno = ENOMEM;
    return -1;
  }
  item->func = func;
  item->obj = obj;
#ifdef _WIN32
  const DWORD key = WG14_SIGNALS_PREFIX(thread_atexit_fls_key)();
  if(key == FLS_OUT_OF_INDEXES)
  {
    free(item);
    errno = ENOMEM;
    return -1;
  }
  item->next = (WG14_SIGNALS_PREFIX(thread_atexit_item_t) *) FlsGetValue(key);
  if(0 == FlsSetValue(key, item))
  {
    free(item);
    errno = ENOMEM;
    return -1;
  }
#else
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  if(0 != pthread_once(&once, WG14_SIGNALS_PREFIX(thread_atexit_key_init)) ||
     WG14_SIGNALS_PREFIX(thread_atexit_key) == (pthread_key_t) -1)
  {
    free(item);
    errno = ENOMEM;
    return -1;
  }
  item->next = WG14_SIGNALS_PREFIX(thread_atexit_items);
  WG14_SIGNALS_PREFIX(thread_atexit_items) = item;
  // Keep the key's value non-NULL so the destructor runs at thread exit.
  if(0 != pthread_setspecific(WG14_SIGNALS_PREFIX(thread_atexit_key),
                              WG14_SIGNALS_PREFIX(thread_atexit_items)))
  {
    WG14_SIGNALS_PREFIX(thread_atexit_items) = item->next;
    free(item);
    errno = ENOMEM;
    return -1;
  }
#endif
  return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
