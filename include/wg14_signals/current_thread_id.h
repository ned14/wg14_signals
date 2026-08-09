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

#ifndef WG14_SIGNALS_GET_TID_H
#define WG14_SIGNALS_GET_TID_H

#include "config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  //! \brief The type of a thread id
  typedef uintptr_t WG14_SIGNALS_PREFIX(thread_id_t);

  static const WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(thread_id_t_tombstone) = 0;

#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
  // If this platform has async signal safe thread locals, we can cache the
  // thread id in an inlineable variable. current_thread_id_cached is
  // deliberately a single symbol shared across ALL translation units of the
  // program, not a per-TU copy: it holds the one cached current thread id,
  // written by internal_current_thread_id_cached_set() and read by
  // current_thread_id(). The declaration below is extern first and the
  // definition lives separately in current_thread_id.c.ipp; in header-only mode
  // every TU provides a weak/selectany definition that the linker merges into
  // one. (Variables cannot be dllexported, so the symbol is not exported from
  // Windows DLLs; WG14_SIGNALS_DEFAULT_VISIBILITY only affects non-Windows
  // shared libraries, keeping the symbol visible under -fvisibility=hidden.)
  extern WG14_SIGNALS_DEFAULT_VISIBILITY
#if defined(_WIN32) || WG14_SIGNALS_ENABLE_HEADER_ONLY
  // Weak/selectany: on Windows thread local vars can't be dllexported, so
  // cache per DLL; in header-only mode every TU provides a definition that the
  // linker merges into one.
  WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS
#endif
  WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(current_thread_id_cached);
#endif

  WG14_SIGNALS_EXTERN WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(internal_current_thread_id_cached_set)(void);

  //! \brief THREADSAFE; ASYNC SIGNAL SAFE; Retrieve the current thread id
  static WG14_SIGNALS_INLINE WG14_SIGNALS_PREFIX(thread_id_t)
  WG14_SIGNALS_PREFIX(current_thread_id)(void)
  {
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
    if(WG14_SIGNALS_PREFIX(current_thread_id_cached) ==
       WG14_SIGNALS_PREFIX(thread_id_t_tombstone))
    {
      WG14_SIGNALS_PREFIX(current_thread_id_cached) =
      WG14_SIGNALS_PREFIX(internal_current_thread_id_cached_set)();
    }
    return WG14_SIGNALS_PREFIX(current_thread_id_cached);
#else
  return WG14_SIGNALS_PREFIX(internal_current_thread_id_cached_set)();
#endif
  }

#ifdef __cplusplus
}
#endif

#if WG14_SIGNALS_ENABLE_HEADER_ONLY
#include "detail/impl/current_thread_id.c.ipp"
#endif

#endif
