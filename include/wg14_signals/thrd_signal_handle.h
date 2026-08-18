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
  // MSVC may be missing necessary signal support, so the sigset helpers are
  // defined here. The N3924 synopses (7.14.2.1/7.14.2.2) declare
  // sigemptyset/sigfillset/sigaddset/sigdelset as `int` "always returns zero"
  // and sigismember as "positive one if set, zero if not", so these match the
  // proposal exactly. On POSIX the four `int` helpers are the host libc's (from
  // <signal.h>), which follow the POSIX contract -- 0 on success, but -1 with
  // errno = EINVAL for an out-of-range signo in sigaddset/sigdelset -- a
  // documented divergence from the proposal's "always returns zero" that only
  // matters for out-of-range input (plans/analysis.md VSDT).
  typedef uint32_t sigset_t;
  static inline int sigemptyset(sigset_t *ss)
  {
    *ss = 0;
    return 0;
  }
  static inline int sigfillset(sigset_t *ss)
  {
    *ss = UINT32_MAX;
    return 0;
  }
  // The shifts below are bounds-checked against the 32-signal bit set
  // (analysis.md 4.5): for signo outside [1, 32], 1u << (signo - 1) is
  // undefined behaviour. sigaddset/sigdelset become no-ops out of range (and
  // still "always return zero", matching the proposal rather than the POSIX
  // -1/EINVAL divergence); sigismember is kept total (returns false) so the
  // Windows sigfillset_* lazy-init checks never read a torn set
  // (plans/ideas.md 4.3). These helpers run in signal-handler context too, so
  // no error reporting.
  static inline int sigaddset(sigset_t *ss, const int signo)
  {
    if(signo >= 1 && signo <= 32)
    {
      *ss |= (1u << (signo - 1));
    }
    return 0;
  }
  static inline int sigdelset(sigset_t *ss, const int signo)
  {
    if(signo >= 1 && signo <= 32)
    {
      *ss &= ~(1u << (signo - 1));
    }
    return 0;
  }
  static inline bool sigismember(const sigset_t *ss, const int signo)
  {
    return (signo >= 1 && signo <= 32) && (*ss & (1u << (signo - 1))) != 0;
  }

  // The 32-signal bit-set scheme above (sigfillset == UINT32_MAX, shifts
  // 1..32, plans/ideas.md 4.3) requires the redefinition to be at least 32
  // bits wide; fail the build if it ever shrinks (plans/ideas.md 4.2).
  WG14_SIGNALS_STATIC_ASSERT((sizeof(sigset_t) >= sizeof(uint32_t)),
                             "wg14_signals: Windows sigset_t must be at least "
                             "32 bits to hold the 32-signal bit set");

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
#define WG14_SIGNALS_SIGFENCE_GLUE(x, y) x y
#define WG14_SIGNALS_SIGFENCE_RETURN_ARG_COUNT(_1_, _2_, _3_, _4_, _5_, _6_,   \
                                               _7_, _8_, count, ...)           \
  count
#define WG14_SIGNALS_SIGFENCE_EXPAND_ARGS(args)                                \
  WG14_SIGNALS_SIGFENCE_RETURN_ARG_COUNT args

// The argument counting below uses __VA_OPT__ only for the zero-argument
// sigfence() form (the comma-suppression case it exists for). __VA_OPT__ is
// C23/C++20, provided as an extension by GCC/Clang in all modes and by MSVC's
// conforming preprocessor in C++20 and C11/C17 modes only. MSVC in C++14/17
// mode has no __VA_OPT__ at all, so use a plain comma-list counting there:
// it dispatches 1..8 arguments correctly (and the compile-time assert below
// checks exactly those), while the zero-argument sigfence() form is
// unavailable on such compilers (plans/analysis.md 4.10).
#if defined(__GNUC__) || defined(__clang__)
#define WG14_SIGNALS_HAVE_VA_OPT 1
#elif defined(_MSC_VER) && defined(_MSVC_TRADITIONAL) &&                       \
(0 == _MSVC_TRADITIONAL)
#if defined(__cplusplus)
#if defined(_MSVC_LANG) && (_MSVC_LANG >= 202002L)
#define WG14_SIGNALS_HAVE_VA_OPT 1
#endif
#else
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define WG14_SIGNALS_HAVE_VA_OPT 1
#endif
#endif
#endif
#ifdef WG14_SIGNALS_HAVE_VA_OPT
#define WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(...)                             \
  WG14_SIGNALS_SIGFENCE_EXPAND_ARGS(                                           \
  (__VA_ARGS__ __VA_OPT__(, ) 8, 7, 6, 5, 4, 3, 2, 1, 0))
#else
#define WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(...)                             \
  WG14_SIGNALS_SIGFENCE_EXPAND_ARGS((__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#endif
#define WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO2(name, count) name##count
#define WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO1(name, count)                     \
  WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO2(name, count)
#define WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO(name, count)                      \
  WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO1(name, count)
#define WG14_SIGNALS_SIGFENCE_CALL_OVERLOAD(name, ...)                         \
  WG14_SIGNALS_SIGFENCE_GLUE(                                                  \
  WG14_SIGNALS_SIGFENCE_OVERLOAD_MACRO(                                        \
  name, WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(__VA_ARGS__)),                   \
  (__VA_ARGS__))

  // The arg counting above depends on __VA_OPT__ (C23/C++20, supported as a
  // GNU/Clang/MSVC extension in older modes). Verify at compile time that the
  // counting machinery returns the right counts: on a compiler without
  // __VA_OPT__ the expansion below is a hard preprocessing error, and on a
  // compiler whose counting is broken the assertion fails — either way the
  // defect surfaces instead of silently mis-dispatching. (The zero-argument
  // path — the comma-suppression case __VA_OPT__ exists for — is exercised by
  // test/sigfence_fence_test.c, whose TU suppresses the -Wpedantic diagnostic
  // that an empty variadic-macro call triggers; a public header must not.
  // The counts asserted here — 3 and 5 — are in the 1..8 range that the
  // non-__VA_OPT__ fallback counting also handles, so the assert is valid on
  // every supported compiler, including MSVC C++14/17.)
  WG14_SIGNALS_STATIC_ASSERT(
  ((WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(a, b, c) == 3) &&
   (WG14_SIGNALS_SIGFENCE_COUNT_ARGS_MAX8(a, b, c, d, e) == 5)),
  "wg14_signals: sigfence() argument counting is broken (requires __VA_OPT__ "
  "on this compiler, or a counting-broken preprocessor)");

#if (defined(__GNUC__) || defined(__clang__)) && !defined(DISABLE_INLINE_ASM)
// On compilers with extended inline asm, we can tell the compiler that a
// specific list of variables must be specifically written out and reloaded
// around the fence. You may find https://godbolt.org/z/chh8ee6Mj useful to
// review. DISABLE_INLINE_ASM (defined by cmake/filc-toolchain.cmake for the
// Fil-C memory-safe compiler) selects the portable volatile-sink fallback
// below instead, because Fil-C cannot compile these asm forms (analysis.md
// AA2).
#define WG14_SIGNALS_SIGFENCE_IMPL_0() __asm__ volatile(";" : : : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_1(a)                                        \
  __asm__ volatile(";" : "+m"(a) : : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_2(a, b)                                     \
  __asm__ volatile(";" : "+m"(a), "+m"(b) : : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_3(a, b, c)                                  \
  __asm__ volatile(";" : "+m"(a), "+m"(b), "+m"(c) : : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_4(a, b, c, d)                               \
  __asm__ volatile(";" : "+m"(a), "+m"(b), "+m"(c), "+m"(d) : : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_5(a, b, c, d, e)                            \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e)               \
                   :                                                           \
                   : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_6(a, b, c, d, e, f)                         \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f)      \
                   :                                                           \
                   : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_7(a, b, c, d, e, f, g)                      \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f),     \
                     "+m"(g)                                                   \
                   :                                                           \
                   : "memory")
#define WG14_SIGNALS_SIGFENCE_IMPL_8(a, b, c, d, e, f, g, h)                   \
  __asm__ volatile(";"                                                         \
                   : "+m"(a), "+m"(b), "+m"(c), "+m"(d), "+m"(e), "+m"(f),     \
                     "+m"(g), "+m"(h)                                          \
                   :                                                           \
                   : "memory")
#else
  // Compilers without extended inline asm (e.g. MSVC), or with it disabled via
  // -DDISABLE_INLINE_ASM (Fil-C): force the listed local variables to be
  // memory-resident, and their values reloaded afterwards, as the "+m" operands
  // and "memory" clobber above do.
  // WG14_SIGNALS_SIGFENCE_ESCAPE() (1) stores each variable's address into a
  // volatile sink so the address escapes to observable memory, and (2) performs
  // a volatile read of one byte of the object -- char may alias any object
  // (C11 6.5p7), and volatile accesses are observable behaviour (C11 5.1.2.3),
  // so no optimizer, link-time code generation (/GL /LTCG) included, may
  // eliminate or reorder them: the value is committed to memory before the
  // fence and must be reloaded after it. No out-of-line function is needed, so
  // the fence cannot be defeated by the optimizer inlining a helper away, and
  // the sink is per-TU static, so header-only consumers need no library
  // symbols.
  static void *volatile WG14_SIGNALS_PREFIX(sigfence_sink)[9];
#define WG14_SIGNALS_SIGFENCE_BARRIER()                                        \
  ((void) (WG14_SIGNALS_PREFIX(sigfence_sink)[8] =                             \
           WG14_SIGNALS_PREFIX(sigfence_sink)[8]))
#define WG14_SIGNALS_SIGFENCE_ESCAPE(a, i)                                     \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_PREFIX(sigfence_sink)[(i)] = (void *) &(a);                   \
    (void) *(volatile unsigned char *) &(a);                                   \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_0() WG14_SIGNALS_SIGFENCE_BARRIER()
#define WG14_SIGNALS_SIGFENCE_IMPL_1(a)                                        \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_2(a, b)                                     \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_3(a, b, c)                                  \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_4(a, b, c, d)                               \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(d, 3);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_5(a, b, c, d, e)                            \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(d, 3);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(e, 4);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_6(a, b, c, d, e, f)                         \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(d, 3);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(e, 4);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(f, 5);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_7(a, b, c, d, e, f, g)                      \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(d, 3);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(e, 4);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(f, 5);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(g, 6);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#define WG14_SIGNALS_SIGFENCE_IMPL_8(a, b, c, d, e, f, g, h)                   \
  do                                                                           \
  {                                                                            \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
    WG14_SIGNALS_SIGFENCE_ESCAPE(a, 0);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(b, 1);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(c, 2);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(d, 3);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(e, 4);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(f, 5);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(g, 6);                                        \
    WG14_SIGNALS_SIGFENCE_ESCAPE(h, 7);                                        \
    WG14_SIGNALS_SIGFENCE_BARRIER();                                           \
  } while(0)
#endif
//! \brief A compiler-only memory barrier, including for local variables in the
//! argument list. Any variable in the argument list MUST be a lvalue.
#define sigfence(...)                                                          \
  WG14_SIGNALS_SIGFENCE_CALL_OVERLOAD(WG14_SIGNALS_SIGFENCE_IMPL_, __VA_ARGS__)
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
  typedef long WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t);
#else
typedef int WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t);
#endif

//! \brief A placeholder type for an OS specific `siginfo_t *` (POSIX) or
//! `PEXCEPTION_RECORD` (Windows)
#ifdef _WIN32
  typedef struct _EXCEPTION_RECORD WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#elif __GLIBC__
typedef siginfo_t WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t);
#elif defined(__FILC__)
// Fil-C's libc siginfo_t is its own complete type, unrelated to the
// struct __siginfo that other POSIX libcs expose, so forward-declaring that
// struct would make every siginfo interaction in the POSIX backend a
// compile-time error. Alias directly to the type the signal handler receives.
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

  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t);
  struct WG14_SIGNALS_PREFIX(sighandler_info);
  struct WG14_SIGNALS_PREFIX(global_signal_decider_t);

  /*! \struct stdc_siginfo
  \brief A platform independent subset of `siginfo_t`.
  */
  struct WG14_SIGNALS_PREFIX(stdc_siginfo)
  {
    int signo;  //!< The signal raised

    //! \brief The system specific error code for this signal, the `si_errno`
    //! code (POSIX) or `NTSTATUS` code (Windows). Zero when the raise carried
    //! no OS info (e.g. `stdc_raise(signo, NULL, NULL)`).
    WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t) error_code;
    void *addr;  //!< Memory location which caused fault, if appropriate. NULL
                 //!< when the raise carried no OS info.
    union WG14_SIGNALS_PREFIX(
    stdc_siginfo_value) value;  //!< A user-defined value

    //! \brief The OS specific signal info
    WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * raw_info;
    //! \brief The OS specific `ucontext_t` (POSIX) or `PCONTEXT` (Windows)
    WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context;
    //! \note On POSIX, a `stdc_raise(signo, NULL, NULL)` sets `raw_info` to
    //! NULL and `raw_context` to the passed `raw_context` (NULL there); on
    //! Windows the OS info is always present (`raw_info` points at the
    //! `EXCEPTION_RECORD`).

    // Used internally only
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *
    internal_local_decider;
    struct WG14_SIGNALS_PREFIX(sighandler_info) * internal_sighandler;
    struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *
    internal_global_decider;
#ifdef _WIN32
    void *internal_win_state;
#endif
    bool internal_decider_is_abandoned;
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
    WG14_SIGNALS_PREFIX(sig_decision_call_recovery)
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

  If you will never return from `decider`,
  you must call `sigdecider_abandon()` to let the runtime clean up its state.
  If after abandonment you realise that you actually shall return, you can
  call `sigdecider_abandon_resume()` to undo the abandonment.
  */
  WG14_SIGNALS_EXTERN union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
  WG14_SIGNALS_PREFIX(sigguarded)(const sigset_t *signals,
                                  WG14_SIGNALS_PREFIX(sig_func_t) guarded,
                                  WG14_SIGNALS_PREFIX(sig_recover_t) recovery,
                                  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
                                  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
                                  value);

  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Lets the decider machinery know you
  won't be returning into it. Can be called from within `sigguarded()`'s
  `decider` functions or global deciders. You must NOT call this from within a
  recovery.

  If called within a local decider, it MUST be the topmost `sigguarded()` for
  the current thread, and it will effectively abandon the current
  `sigguarded()`.

  \param rsi The siginfo passed to the decider function.
  */
  WG14_SIGNALS_EXTERN void WG14_SIGNALS_PREFIX(sigdecider_abandon)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Undoes a prior call of
  `sigdecider_abandon()`. Do not call except from the same decider function
  previously abandoned.

  \param rsi The siginfo passed to the decider function.
 */
  WG14_SIGNALS_EXTERN void WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi);

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
  If that hand-off reaches a previously installed `SA_SIGINFO` handler while
  `raw_info` is NULL (e.g. `stdc_raise(signo, NULL, NULL)`), a minimal
  zeroed `siginfo_t` with `si_signo` set to `signo` and `si_code` set to
  `SI_USER` is synthesised and passed instead of NULL, so a handler that reads
  `si->si_signo`/`si->si_code` without a NULL check does not crash;
  `raw_context` is passed through unchanged and may be NULL.

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
  thread local state is set up and return immediately doing nothing else. The
  call returns `false` both when no decider claims the signal and when the
  per-thread setup fails; on setup failure `errno` is set (e.g. `ENOMEM`) so a
  caller of the setup form (`signo == 0`) can detect that setup actually failed
  (plans/analysis.md 3.7).
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
  Passing a handle that has already been destroyed is undefined behaviour: this
  reference implementation deliberately does not guard against it
  (plans/analysis.md `DEDE`).
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
