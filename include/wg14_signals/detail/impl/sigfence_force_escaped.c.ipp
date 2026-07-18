/* Proposed WG14 improved signals support
(C) 2026 Niall Douglas <http://www.nedproductions.biz/>
File Created: July 2026


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

#include "../../thrd_signal_handle.h"

#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO

#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif
  void WG14_SIGNALS_PREFIX(sigfence_force_escaped)(int x, ...)
  {
    // This appears to be enough to prevent inlining of implementation
    // not forcing the compiler to consider passed arguments as escaped
    (void) x;
    va_list args;
    va_start(args, x);
    va_end(args);
  }
#ifdef __cplusplus
}
#endif

#endif
