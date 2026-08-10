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
   is used preferentially and the fallbacks below are excluded. Its return
   value is deliberately ignored: on some platforms (e.g. macOS) it is not
   reliable even though the registration works, so thread_atexit() reports
   success once the callback has been handed to the runtime.

   Otherwise the callbacks are drained at thread exit via the platform's
   thread-exit hook: pthread_key destructors on POSIX, and the
   IMAGE_TLS_DIRECTORY TLS callback array (.CRT$XLB) on MSVC -- the same
   mechanism the MSVC CRT uses for __declspec(thread) destructors.

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
  // A data symbol, not a function: ISO C forbids converting a function pointer
  // to an object pointer type, and the address is only used as a unique DSO
  // identity token by the runtime.
  static int WG14_SIGNALS_PREFIX(thread_atexit_dso_symbol);

  //! \brief Register a callback to run when the calling thread exits.
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(thread_atexit)(void (*func)(void *obj), void *obj)
  {
    // The return value is not reliable on every platform that supplies the
    // symbol (e.g. macOS returns garbage while the registration works), so it
    // is ignored.
    __cxa_thread_atexit(func, obj,
                        &WG14_SIGNALS_PREFIX(thread_atexit_dso_symbol));
    return 0;
  }
#else
#include <stdlib.h>

#if defined(_WIN32) && defined(_MSC_VER)
// MSVC-compatible compilers (MSVC and clang-cl; both define _MSC_VER, ordinary
// GNU-mode clang and MinGW do not): drain the registered callbacks at thread
// exit through the IMAGE_TLS_DIRECTORY TLS callback array. The callback below
// is folded into the .CRT$XLB section, which the linker collects into the PE
// TLS callback array, so Windows invokes it at DLL_THREAD_DETACH /
// DLL_PROCESS_DETACH -- the same mechanism the MSVC CRT uses for
// __declspec(thread) destructors (crt/src/vcruntime/tlsdtor.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct WG14_SIGNALS_PREFIX(thread_atexit_item_t)
{
  void (*func)(void *obj);
  void *obj;
  struct WG14_SIGNALS_PREFIX(thread_atexit_item_t) * next;
} WG14_SIGNALS_PREFIX(thread_atexit_item_t);

static __declspec(thread) WG14_SIGNALS_PREFIX(thread_atexit_item_t) *
WG14_SIGNALS_PREFIX(thread_atexit_items) = WG14_SIGNALS_NULLPTR;

static void NTAPI WG14_SIGNALS_PREFIX(thread_atexit_tls_callback)(
PVOID hDll, DWORD reason, PVOID reserved)
{
  (void) hDll;
  (void) reserved;
  if(reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH)
  {
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
}

// Fold the callback into the PE TLS callback array. .CRT$XLB runs before the
// CRT's own __dyn_tls_dtor (in .CRT$XLD).
#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) static PIMAGE_TLS_CALLBACK WG14_SIGNALS_PREFIX(
thread_atexit_tls_cb) = WG14_SIGNALS_PREFIX(thread_atexit_tls_callback);

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
  item->next = WG14_SIGNALS_PREFIX(thread_atexit_items);
  WG14_SIGNALS_PREFIX(thread_atexit_items) = item;
  return 0;
}
#elif defined(_WIN32)
// Other Windows compilers (ordinary GNU-mode clang, MinGW): MinGW supplies
// __cxa_thread_atexit() through its winpthreads runtime (and is otherwise not
// supported by this project); the TLS-directory fallback requires the MSVC
// ABI.
#error                                                                         \
"thread_atexit(): unsupported Windows compiler without __cxa_thread_atexit() (MSVC or clang-cl required)"
#else
#include <pthread.h>

typedef struct WG14_SIGNALS_PREFIX(thread_atexit_item_t)
{
  void (*func)(void *obj);
  void *obj;
  struct WG14_SIGNALS_PREFIX(thread_atexit_item_t) * next;
} WG14_SIGNALS_PREFIX(thread_atexit_item_t);

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
  return 0;
}
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
