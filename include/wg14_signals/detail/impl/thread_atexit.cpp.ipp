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

/* The C++ implementation of thread_atexit(), selected (in preference to the C
   implementation) only on platforms that do NOT supply __cxa_thread_atexit().
   It uses a thread_local std::vector whose element destructors run at thread
   exit. When __cxa_thread_atexit() is available the C implementation is used
   instead and this file is neither compiled nor linked (see
   thread_atexit.h and CMakeLists.txt). */

#ifndef WG14_SIGNALS_THREAD_ATEXIT_CPP_IPP
#define WG14_SIGNALS_THREAD_ATEXIT_CPP_IPP

#include "thread_atexit.h"

#include <cerrno>
#include <vector>

extern "C"
{
  // WG14_SIGNALS_EXTERN is static inline in header-only mode so this matches
  // the header's declaration and every TU gets its own copy (analysis.md 1.8).
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(thread_atexit)(void (*func)(void *obj), void *obj)
  {
    struct item
    {
      void (*func)(void *obj);
      void *obj;

      constexpr item(void (*_func)(void *obj), void *_obj)
          : func(_func)
          , obj(_obj)
      {
      }
      item(const item &) = delete;
      item(item &&o) noexcept
          : func(o.func)
          , obj(o.obj)
      {
        o.func = nullptr;
        o.obj = nullptr;
      }
      item &operator=(const item &) = delete;
      item &operator=(item &&) = delete;
      ~item()
      {
        if(func != nullptr)
        {
          func(obj);
          func = nullptr;
        }
      }
    };
#ifdef __cpp_exceptions
    try
#endif
    {
      static thread_local std::vector<item> items;
      items.emplace_back(func, obj);
      return 0;
    }
#ifdef __cpp_exceptions
    catch(...)
    {
      errno = ENOMEM;
      return -1;
    }
#endif
  }
}

#endif
