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

#ifndef WG14_SIGNALS_CURRENT_THREAD_ID_IPP
#define WG14_SIGNALS_CURRENT_THREAD_ID_IPP

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../../current_thread_id.h"

#ifdef __linux__
#include <sys/syscall.h>  // for SYS_gettid
#include <unistd.h>       // for syscall()
#endif

#if defined(__APPLE__)
#include <mach/mach_init.h>  // for mach_thread_self
#include <mach/mach_port.h>  // for mach_port_deallocate
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>  // for pthread_getthreadid_np
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
  extern __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
  // The single shared definition of the cached thread id (declared extern in
  // current_thread_id.h, so this file-scope definition has external linkage
  // without needing a redundant 'extern' keyword). Visibility is forced to
  // default so the symbol is exported from a shared library even under
  // -fvisibility=hidden.
  WG14_SIGNALS_DEFAULT_VISIBILITY
#if WG14_SIGNALS_ENABLE_HEADER_ONLY
  // In header-only mode every translation unit defines this TLS variable; the
  // weak/selectany linkage lets the linker merge the copies (analysis.md Y10).
  WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS
#endif
  WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(current_thread_id_cached) =
#ifdef _WIN32
  0;
#else
  WG14_SIGNALS_PREFIX(thread_id_t_tombstone);
#endif
#endif

  static inline WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(get_current_thread_id)(void)
  {
#ifdef _WIN32
    return (WG14_SIGNALS_PREFIX(thread_id_t)) GetCurrentThreadId();
#elif defined(__linux__)
  return (WG14_SIGNALS_PREFIX(thread_id_t)) syscall(SYS_gettid);
#elif defined(__APPLE__)
  thread_port_t tid = mach_thread_self();
  mach_port_deallocate(mach_task_self(), tid);
  return (WG14_SIGNALS_PREFIX(thread_id_t)) tid;
#else
  return (WG14_SIGNALS_PREFIX(thread_id_t)) pthread_getthreadid_np();
#endif
  }

  WG14_SIGNALS_EXTERN WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(internal_current_thread_id_cached_set)(void)
  {
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
    WG14_SIGNALS_PREFIX(current_thread_id_cached) =
    WG14_SIGNALS_PREFIX(get_current_thread_id)();
    return WG14_SIGNALS_PREFIX(current_thread_id_cached);
#else
  return WG14_SIGNALS_PREFIX(get_current_thread_id)();
#endif
  }

#ifdef __cplusplus
}
#endif

#endif
