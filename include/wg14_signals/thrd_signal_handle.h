/* Proposed WG14 improved signals support
(C) 2025 - 2026 Niall Douglas <http://www.nedproductions.biz/>
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

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#if __GLIBC__
// glibc defines a userspace siginfo_t as an unnamed struct. No
// choice here but to drag in the relevant header.
#include <bits/types/siginfo_t.h>
#endif


#ifdef __cplusplus
extern "C"
{
#endif

#ifdef _WIN32
  // MSVC may be missing necessary signal support
  typedef uint32_t sigset_t;
  static inline void sigemptyset(sigset_t *ss)
  {
    *ss = 0;
  }
  static inline void sigfillset(sigset_t *ss)
  {
    *ss = UINT32_MAX;
  }
  static inline void sigaddset(sigset_t *ss, const int signo)
  {
    *ss |= (1u << (signo - 1));
  }
  static inline void sigdelset(sigset_t *ss, const int signo)
  {
    *ss &= ~(1u << (signo - 1));
  }
  static inline bool sigismember(const sigset_t *ss, const int signo)
  {
    return (*ss & (1u << (signo - 1))) != 0;
  }

// MSVC appears to follow the Linux signal numbering
#ifndef SIGBUS
#define SIGBUS (7)
#endif
#ifndef SIGKILL
#define SIGKILL (9)
#endif
#ifndef SIGSTOP
#define SIGSTOP (19)
#endif
#endif

#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO
  WG14_SIGNALS_EXTERN void WG14_SIGNALS_PREFIX(sigfence_force_escaped)(int,
                                                                       ...);
#define SIGFENCE_GLUE(x, y) x y
#define SIGFENCE_RETURN_ARG_COUNT(_1_, _2_, _3_, _4_, _5_, _6_, _7_, _8_,      \
                                  count, ...)                                  \
  count
#define SIGFENCE_EXPAND_ARGS(args) SIGFENCE_RETURN_ARG_COUNT args
#define SIGFENCE_COUNT_ARGS_MAX8(...)                                          \
  SIGFENCE_EXPAND_ARGS((__VA_ARGS__ __VA_OPT__(, ) 8, 7, 6, 5, 4, 3, 2, 1, 0))
#define SIGFENCE_OVERLOAD_MACRO2(name, count) name##count
#define SIGFENCE_OVERLOAD_MACRO1(name, count)                                  \
  SIGFENCE_OVERLOAD_MACRO2(name, count)
#define SIGFENCE_OVERLOAD_MACRO(name, count)                                   \
  SIGFENCE_OVERLOAD_MACRO1(name, count)
#define SIGFENCE_CALL_OVERLOAD(name, ...)                                      \
  SIGFENCE_GLUE(                                                               \
  SIGFENCE_OVERLOAD_MACRO(name, SIGFENCE_COUNT_ARGS_MAX8(__VA_ARGS__)),        \
  (__VA_ARGS__))

#if defined(__GNUC__) || defined(__clang__)
// On compilers with extended inline asm, we can tell the compiler that a
// specific list of variables must be specifically written out and reloaded
// around the fence. You may find https://godbolt.org/z/chh8ee6Mj useful to
// review.
#define SIGFENCE_IMPL_0() __asm__ volatile(";" : : : "memory")
#define SIGFENCE_IMPL_1(a) __asm__ volatile(";" : "+m"(a) : : "memory")
#define SIGFENCE_IMPL_2(a, b)                                                  \
  __asm__ volatile(";" : "+m"(a), "+m"(b) : : "memory")
#define SIGFENCE_IMPL_3(a, b, c)                                               \
  __asm__ volatile(";" : "+m"(a), "+m"(b), "+m"(c) : : "memory")
#define SIGFENCE_IMPL_4(a, b, c, d)                                            \
  __asm__ volatile(";" : "+m"(a), "+m"(b), "+m"(c), "+m"(d) : : "memory")
#define SIGFENCE_IMPL_5(a, b, c, d, e)                                         \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e)               \
                   :                                                           \
                   : "memory")
#define SIGFENCE_IMPL_6(a, b, c, d, e, f)                                      \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f)      \
                   :                                                           \
                   : "memory")
#define SIGFENCE_IMPL_7(a, b, c, d, e, f, g)                                   \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f),     \
                     "+m"(g)                                                   \
                   :                                                           \
                   : "memory")
#define SIGFENCE_IMPL_8(a, b, c, d, e, f, g, h)                                \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f),     \
                     "+m"(g), "+m"(h)                                          \
                   :                                                           \
                   : "memory")
#else
  // This is a much less efficient way of forcing the compiler to treat
  // variables as having escaped, but it's portable.
#define SIGFENCE_IMPL_0() WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0)
#define SIGFENCE_IMPL_1(a) WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a))
#define SIGFENCE_IMPL_2(a, b)                                                  \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b))
#define SIGFENCE_IMPL_3(a, b, c)                                               \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c))
#define SIGFENCE_IMPL_4(a, b, c, d)                                            \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c), &(d))
#define SIGFENCE_IMPL_5(a, b, c, d, e)                                         \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c), &(d), &(e))
#define SIGFENCE_IMPL_6(a, b, c, d, e, f)                                      \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c), &(d), &(e), \
                                              &(f))
#define SIGFENCE_IMPL_7(a, b, c, d, e, f, g)                                   \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c), &(d), &(e), \
                                              &(f), &(g))
#define SIGFENCE_IMPL_8(a, b, c, d, e, f, g, h)                                \
  WG14_SIGNALS_PREFIX(sigfence_force_escaped)(0, &(a), &(b), &(c), &(d), &(e), \
                                              &(f), &(g), &(h))
#endif
//! \brief A compiler-only memory barrier, including for local variables in the
//! argument list.
#define sigfence(...) SIGFENCE_CALL_OVERLOAD(SIGFENCE_IMPL_, __VA_ARGS__)
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4190)  // C-linkage with UDTs
#endif
#if defined(__clang__) && defined(__cplusplus)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

  /*! \union stdc_siginfo_value
  \brief User defined value.
  */
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
  {
    intptr_t int_value;
    void *ptr_value;
#if defined(__cplusplus)
    constexpr WG14_SIGNALS_PREFIX(stdc_siginfo_value)()
        : int_value(0)
    {
    }
    constexpr WG14_SIGNALS_PREFIX(stdc_siginfo_value)(int v)
        : int_value(v)
    {
    }
    constexpr WG14_SIGNALS_PREFIX(stdc_siginfo_value)(void *v)
        : ptr_value(v)
    {
    }
#endif
  };
  //! \brief Typedef to a system specific error code type
#ifdef _WIN32
  typedef long WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t);
#else
typedef int WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t);
#endif

//! \brief A placeholder type for an OS specific `siginfo_t *` (POSIX) or
//! `PEXCEPTION_RECORD` (Windows)
#ifdef _WIN32
  typedef struct _EXCEPTION_RECORD WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#elif __GLIBC__
typedef siginfo_t WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#elif __ANDROID__
typedef struct siginfo WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#else
typedef struct __siginfo WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#endif

  //! \brief A placeholder type for an OS specific `ucontext_t` (POSIX) or
  //! `PCONTEXT` (Windows)
#ifdef _WIN32
  typedef struct _CONTEXT WG14_SIGNALS_PREFIX(stdc_siginfo_context_t);
#else
typedef ucontext_t WG14_SIGNALS_PREFIX(stdc_siginfo_context_t);
#endif

  /*! \struct stdc_siginfo
  \brief A platform independent subset of `siginfo_t`.
  */
  struct WG14_SIGNALS_PREFIX(stdc_siginfo)
  {
    int signo;  //!< The signal raised

    //! \brief The system specific error code for this signal, the `si_errno`
    //! code (POSIX) or `NTSTATUS` code (Windows)
    WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t) error_code;
    void *addr;  //!< Memory location which caused fault, if appropriate
    union WG14_SIGNALS_PREFIX(
    stdc_siginfo_value) value;  //!< A user-defined value

    //! \brief The OS specific signal info
    WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * raw_info;
    //! \brief The OS specific `ucontext_t` (POSIX) or `PCONTEXT` (Windows)
    WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context;
  };

  //! \brief The type of the guarded function.
  typedef union WG14_SIGNALS_PREFIX(stdc_siginfo_value)(WG14_SIGNALS_PREFIX(
  sig_func_t))(union WG14_SIGNALS_PREFIX(stdc_siginfo_value));

  //! \brief The type of the function called to recover from a signal being
  //! raised in a guarded section.
  typedef union WG14_SIGNALS_PREFIX(stdc_siginfo_value)(WG14_SIGNALS_PREFIX(
  sig_recover_t))(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) *);

  //! \brief The decision taken by the decider function
  enum WG14_SIGNALS_PREFIX(sig_decision_t)
  {
    //! \brief We have decided to do nothing
    WG14_SIGNALS_PREFIX(sig_decision_next_decider),
    //! \brief We have fixed the cause of the signal, please resume execution
    WG14_SIGNALS_PREFIX(sig_decision_resume_execution),
    //! \brief Thread local signal deciders only: reset the stack and local
    //! state to entry to `sigguarded()`, and call the recovery
    //! function.
    WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery)
  };

  //! \brief The type of the function called when a signal is raised. Returns
  //! a decision of how to handle the signal.
  typedef enum WG14_SIGNALS_PREFIX(sig_decision_t)(WG14_SIGNALS_PREFIX(
  sig_decide_t))(struct WG14_SIGNALS_PREFIX(stdc_siginfo) *);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Fills the set of synchronous signals
  for this platform.

  Synchronous signals are those which can be raised by a thread in the course
  of its execution. This set can include platform-specific additions, however
  at least these POSIX signals are within this set:

  * `SIGABRT`
  * `SIGBUS`
  * `SIGFPE`
  * `SIGILL`
  * `SIGPIPE`
  * `SIGSEGV`
  * `SIGSYS`
  */
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(sigfillset_synchronous)(sigset_t *set);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Fills the set of non-debug
  asynchronous signals for this platform.

  Non-debug asynchronous signals are those which are delivered by the system to
  notify the process about some event which does not default to resulting in a
  core dump. This set can include platform-specific additions, however at least
  these POSIX signals are within this set:

  * `SIGALRM`
  * `SIGCHLD`
  * `SIGCONT`
  * `SIGHUP`
  * `SIGINT`
  * `SIGKILL`
  * `SIGSTOP`
  * `SIGTERM`
  * `SIGTSTP`
  * `SIGTTIN`
  * `SIGTTOU`
  * `SIGUSR1`
  * `SIGUSR2`
  * `SIGPOLL`
  * `SIGPROF`
  * `SIGURG`
  * `SIGVTALRM`
  */
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(sigfillset_asynchronous_nondebug)(sigset_t *set);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Fille the set of debug asynchronous
  signals for this platform.

  Debug asynchronous signals are those which are delivered by the system to
  notify the process about some event which defaults to resulting in a core
  dump. This set can include platform-specific additions, however at least these
  POSIX signals are within this set:

  * `SIGQUIT`
  * `SIGTRAP`
  * `SIGXCPU`
  * `SIGXFSZ`
  */
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(sigfillset_asynchronous_debug)(sigset_t *set);

  /*! \brief THREADSAFE USUALLY ASYNC-SIGNAL-SAFE Installs a thread-local signal
  guard for the calling thread, and calls the guarded function `guarded`.

  \return The value returned by `guarded`, or `recovery`.
  \param signals The set of signals to guard against.
  \param guarded A function whose execution is to be guarded against signal
  raises.
  \param recovery A function to be called if a signal is raised.
  \param decider A function to be called to decide whether to
  recover from the signal and continue the execution of the guarded routine, or
  to abort and call the recovery routine.
  \param value A value to supply to the guarded routine.

  By "usually async signal safe" we mean that if any function from this library
  has been called from the called from the calling thread, this is async signal
  safe. If you need to set up this library for a calling thread without doing
  anything else, calling `stdc_raise(0, nullptr, nullptr)`, this will
  ensure the calling thread's thread local state is set up and return
  immediately doing nothing else.
   */
  WG14_SIGNALS_EXTERN union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
  WG14_SIGNALS_PREFIX(sigguarded)(const sigset_t *signals,
                                  WG14_SIGNALS_PREFIX(sig_func_t) guarded,
                                  WG14_SIGNALS_PREFIX(sig_recover_t) recovery,
                                  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
                                  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
                                  value);
#if defined(__clang__) && defined(__cplusplus)
#pragma clang diagnostic pop
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

  /*! \brief THREADSAFE USUALLY ASYNC-SIGNAL-SAFE Call OUR currently installed
  signal decider for a signal (POSIX), or raise a Win32 structured exception
  (Windows), returning false if we have no decider installed for that signal.

  Note that on POSIX, we fetch OUR currently installed signal decider and call
  it directly. This allows us to supply custom `raw_info` and `raw_context`.
  Each decider in our chain will be invoked in turn until we reach whatever the
  signal handler was when this library was first initialised, and we hand off
  to that handler. If that handler was defaulted and the default handling is not
  to ignore, we reset the handler installation and execute
  `pthread_kill(pthread_self(), signo)` in order to invoke the default handling.

  It is important to note that this call does not raise signals itself except in
  that final handling step as just described. Therefore, if your code overwrites
  the signal handlers installed by this library with a custom handler, and you
  wish to pass on signal handling to this library, this is the right API to call
  to do that.

  On Windows, Win32 structured exceptions are capable of being used directly and
  so we do on that platform always call `RaiseException()`.

  By "usually async signal safe" we mean that if any function from this library
  has been called from the called from the calling thread, this is async signal
  safe. If you need to set up this library for a calling thread without doing
  anything else, specify zero for `signo`, this will ensure the calling thread's
  thread local state is set up and return immediately doing nothing else.
  */
  WG14_SIGNALS_EXTERN bool WG14_SIGNALS_PREFIX(stdc_raise)(
  int signo, WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * raw_info,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context);

  /*! \brief THREADSAFE Installs, and potentially enables, the global signal
  handlers for the signals specified by `guarded`. Each signal installed is
  threadsafe reference counted, so this is safe to call from multiple threads or
  instantiate multiple times.

  If `guarded` is null, all the standard POSIX signals are used.

  ## POSIX only

  Any existing global signal handlers are replaced with a filtering signal
  handler, which checks if the current kernel thread has installed a signal
  guard, and if so executes the guard. If no signal guard has been installed for
  the current kernel thread, global signal continuation handlers are executed.
  If none claims the signal, the previously installed signal handler is called.

  After the new signal handlers have been installed, the guarded signals are
  globally enabled for all threads of execution. Be aware that the handlers are
  installed with `SA_NODEFER` to avoid the need to perform an expensive syscall
  when a signal is handled. However this may also produce surprise e.g. infinite
  loops.

  \warning This class is threadsafe with respect to other concurrent executions
  of itself, but is NOT threadsafe with respect to other code modifying the
  global signal handlers.
  */
  WG14_SIGNALS_EXTERN void *
  WG14_SIGNALS_PREFIX(siginstall)(const sigset_t *guarded);
  /*! \brief THREADSAFE Uninstall a previously installed signal guard.
   */
  WG14_SIGNALS_EXTERN int WG14_SIGNALS_PREFIX(siguninstall)(void *i);
  /*! \brief THREADSAFE Uninstall a previously system installed signal guard.
   */
  WG14_SIGNALS_EXTERN int WG14_SIGNALS_PREFIX(siguninstall_system)(int version);

  /*! \brief THREADSAFE NOT REENTRANT Create a global signal continuation
  decider. Threadsafe with respect to other calls of this function, but not
  reentrant i.e. modifying the global signal continuation decider registry
  whilst inside a global signal continuation decider is racy, and in any case
  definitely not async signal handler safe. Called after all
  thread local handling is exhausted. Note that what you can safely do in the
  decider function is extremely limited, only async signal safe functions may be
  called.

  \return An opaque pointer to the registered decider. `NULL` if `malloc`
  failed.
  \param guarded The set of signals to be guarded against.
  \param callfirst True if this decider should be called before any other.
  Otherwise call order is in the order of addition.
  \param decider A decider function, which must return `true` if execution is to
  resume, `false` if the next decider function should be called.
  \param value A user supplied value to set in the `raised_signal_info` passed
  to the decider callback.
  */
  WG14_SIGNALS_EXTERN void *WG14_SIGNALS_PREFIX(signal_decider_create)(
  const sigset_t *guarded, bool callfirst,
  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value);
  /*! \brief THREADSAFE NOT REENTRANT Destroy a global signal continuation
  decider. Threadsafe with respect to other calls of this function, but not
  reentrant i.e. do not call whilst inside a global signal continuation decider.
  \return True if recognised and thus removed.
  */
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(signal_decider_destroy)(void *decider);


#ifdef __cplusplus
}
#endif

#if WG14_SIGNALS_ENABLE_HEADER_ONLY
#ifdef _WIN32
#include "detail/impl/thrd_signal_handle_windows.c.ipp"
#else
#include "detail/impl/thrd_signal_handle_posix.c.ipp"
#endif
#endif


#endif
