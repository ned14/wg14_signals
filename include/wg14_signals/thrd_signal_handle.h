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

#ifndef WG14_SIGNALS_THREAD_LOCAL_SIGNAL_HANDLE_H
#define WG14_SIGNALS_THREAD_LOCAL_SIGNAL_HANDLE_H

#include "config.h"

#include <setjmp.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /*! \union thrd_raised_signal_info_value
  \brief User defined value.
  */
  union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)
  {
    intptr_t int_value;
    void *ptr_value;
#if defined(__cplusplus)
    constexpr WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)()
        : int_value(0)
    {
    }
    constexpr WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)(int v)
        : int_value(v)
    {
    }
    constexpr WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)(void *v)
        : ptr_value(v)
    {
    }
#endif
  };
  //! Typedef to a system specific error code type
#ifdef _WIN32
  typedef long WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t);
#else
typedef int WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t);
#endif

  /*! \struct thrd_raised_signal_info
  \brief A platform independent subset of `siginfo_t`.
  */
  struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info)
  {
    jmp_buf buf;  //!< setjmp() buffer written on entry to guarded section
    int signo;    //!< The signal raised

    //! The system specific error code for this signal, the `si_errno` code (POSIX) or `NTSTATUS` code (Windows)
    WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t) error_code;
    void *addr;  //!< Memory location which caused fault, if appropriate
    union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value;  //!< A user-defined value

    //! The OS specific `siginfo_t *` (POSIX) or `PEXCEPTION_RECORD` (Windows)
    void *raw_info;
    //! The OS specific `ucontext_t` (POSIX) or `PCONTEXT` (Windows)
    void *raw_context;
  };

  //! \brief The type of the guarded function.
  typedef union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) (*WG14_SIGNALS_PREFIX(thrd_signal_func_t))(
  union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value));

  //! \brief The type of the function called to recover from a signal being raised in a guarded section.
  typedef union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) (*WG14_SIGNALS_PREFIX(thrd_signal_recover_t))(
  const struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info) *);

  //! \brief The type of the function called when a signal is raised. Returns true to continue guarded code, false to
  //! recover.
  typedef bool (*WG14_SIGNALS_PREFIX(thrd_signal_decide_t))(struct WG14_SIGNALS_PREFIX(thrd_raised_signal_info) *);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4190)  // C-linkage with UDTs
#endif
  /*! \brief Installs a thread-local signal guard for the calling thread, and calls the guarded function `guarded`.
  \return The value returned by `guarded`, or `recovery`.
  \param signals The set of signals to guard against.
  \param guarded A function whose execution is to be guarded against signal raises.
  \param recovery A function to be called if a signal is raised.
  \param decider A function to be called to decide whether to recover from the signal and continue
  the execution of the guarded routine, or to abort and call the recovery routine.
  \param value A value to supply to the guarded routine.
   */
  WG14_SIGNALS_EXTERN union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value)
  WG14_SIGNALS_PREFIX(thrd_signal_call)(const sigset_t *signals, WG14_SIGNALS_PREFIX(thrd_signal_func_t) guarded,
                                        WG14_SIGNALS_PREFIX(thrd_signal_recover_t) recovery,
                                        WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider,
                                        union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value);
#ifdef _MSC_VER
#pragma warning(pop)
#endif

  /*! \brief Call the currently installed signal handler for a signal (POSIX), or raise a Win32 structured
  exception (Windows), returning false if no handler was called due to the currently
  installed handler being `SIG_IGN` (POSIX).

  Note that on POSIX, we fetch the currently installed signal handler and try to call it directly.
  This allows us to supply custom `raw_info` and `raw_context`, and we do all the things which the signal
  handler flags tell us to do beforehand [1]. If the current handler has been defaulted, we
  enable the signal and execute `pthread_kill(pthread_self(), signo)` in order to invoke the
  default handling.

  Note that on Windows, `raw_context` is ignored as there is no way to override the context thrown
  with a Win32 structured exception.

  [1]: We currently do not implement alternative stack switching. If a handler requests that, we
  simply abort the process. Code donations implementing support are welcome.
   */
  WG14_SIGNALS_EXTERN bool WG14_SIGNALS_PREFIX(thrd_signal_raise)(int signo, void *raw_info, void *raw_context);

  /*! \brief THREADSAFE On platforms where it is necessary (POSIX), installs, and potentially enables,
  the global signal handlers for the signals specified by `guarded`. Each signal installed
  is threadsafe reference counted, so this is safe to call from multiple threads or instantiate
  multiple times.

  On platforms with better than POSIX global signal support, this function does nothing.

  ## POSIX only

  Any existing global signal handlers are replaced with a filtering signal handler, which
  checks if the current kernel thread has installed a signal guard, and if so executes the
  guard. If no signal guard has been installed for the current kernel thread, global signal
  continuation handlers are executed. If none claims the signal, the previously
  installed signal handler is called.

  After the new signal handlers have been installed, the guarded signals are globally enabled
  for all threads of execution. Be aware that the handlers are installed with `SA_NODEFER`
  to avoid the need to perform an expensive syscall when a signal is handled.
  However this may also produce surprise e.g. infinite loops.

  \warning This class is threadsafe with respect to other concurrent executions of itself,
  but is NOT threadsafe with respect to other code modifying the global signal handlers.
  */
  WG14_SIGNALS_EXTERN void *WG14_SIGNALS_PREFIX(modern_signals_install)(const sigset_t *guarded, int version);
  /*! \brief THREADSAFE Uninstall a previously installed signal guard.
   */
  WG14_SIGNALS_EXTERN bool WG14_SIGNALS_PREFIX(modern_signals_uninstall)(void *i);
  /*! \brief THREADSAFE Uninstall a previously system installed signal guard.
   */
  WG14_SIGNALS_EXTERN bool WG14_SIGNALS_PREFIX(modern_signals_uninstall_system)(int version);

  /*! \brief THREADSAFE NOT REENTRANT Create a global signal continuation decider. Threadsafe with respect to
  other calls of this function, but not reentrant i.e. modifying the global signal continuation
  decider registry whilst inside a global signal continuation decider is racy. Called after
  all thread local handling is exhausted. Note that what you can safely do in the decider
  function is extremely limited, only async signal safe functions may be called.

  \return An opaque pointer to the registered decider. `NULL` if `malloc` failed.
  \param guarded The set of signals to be guarded against.
  \param callfirst True if this decider should be called before any other. Otherwise
  call order is in the order of addition.
  \param decider A decider function, which must return `true` if execution is to resume,
  `false` if the next decider function should be called.
  \param value A user supplied value to set in the `raised_signal_info` passed to the
  decider callback.
  */
  WG14_SIGNALS_EXTERN void *
  WG14_SIGNALS_PREFIX(signal_decider_create)(const sigset_t *guarded, bool callfirst,
                                             WG14_SIGNALS_PREFIX(thrd_signal_decide_t) decider,
                                             union WG14_SIGNALS_PREFIX(thrd_raised_signal_info_value) value);
  /*! \brief THREADSAFE NOT REENTRANT Destroy a global signal continuation decider. Threadsafe with
  respect to other calls of this function, but not reentrant i.e. do not call
  whilst inside a global signal continuation decider.
  \return True if recognised and thus removed.
  */
  WG14_SIGNALS_EXTERN bool WG14_SIGNALS_PREFIX(signal_decider_destroy)(void *decider);


#ifdef __cplusplus
}
#endif

#endif
