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

#ifndef WG14_SIGNALS_THRD_SIGNAL_HANDLE_WINDOWS_IPP
#define WG14_SIGNALS_THRD_SIGNAL_HANDLE_WINDOWS_IPP

// A wrong-platform include is a clear compile error instead of a silent
// mis-compile (plans/ideas.md 4.4).
#if !defined(_WIN32) && !defined(_WIN64)
#error "thrd_signal_handle_windows.c.ipp must only be included on Windows"
#endif

// Windows SDK floor (plans/ideas.md 2.3): the backend relies on
// AddVectoredContinueHandler / SetUnhandledExceptionFilter (Vista-and-later SDK
// declarations). The library build defines _WIN32_WINNT/WINVER as 0x0600
// (CMakeLists.txt, PUBLIC); reject an explicitly lower floor here, so a
// consumer that sets _WIN32_WINNT < 0x0600 fails loudly instead of compiling
// against a silently reduced API surface.
#if defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
#error "wg14_signals requires _WIN32_WINNT >= 0x0600 (Windows Vista or later)"
#endif

#include "../../thrd_signal_handle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "thrd_signal_handle_common.ipp.ipp"

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4190)  // C-linkage with UDTs
#pragma warning(disable : 4611)  // Interaction between setjmp and C++
#endif


  static const sigset_t *WG14_SIGNALS_PREFIX(synchronous_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};
    if(sigismember(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    sigemptyset(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      sigaddset(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_synchronous)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(synchronous_sigset)(), sizeof(*set));
    return 0;
  }

  static const sigset_t *WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {SIGINT, SIGKILL, SIGSTOP, SIGTERM};
    if(sigismember(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    sigemptyset(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      sigaddset(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_nondebug)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(),
           sizeof(*set));
    return 0;
  }

  static const sigset_t *WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(void)
  {
    static sigset_t v;
    sigset_t x;
    sigemptyset(&x);
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_debug)(sigset_t *set)
  {
    memcpy(set, WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(), sizeof(*set));
    return 0;
  }


// User-defined exception-code base for stdc_raise() of signals that have no
// native Win32 exception code (SIGINT, SIGTERM, SIGPIPE, SIGUSR1, ...).
// 0x40000000-0x7FFFFFFF is the documented user-defined exception-code range;
// no real Windows exception code (all are >= 0x80000000) collides with it,
// and signal_from_win32_exception_code() reverses the offset so a decider
// installed for such a signal dispatches. stdc_raise() of an unsupported
// signo therefore raises a valid SEH exception and returns false when no
// decider claims it, instead of abort()ing (analysis.md 2.12/V4).
#define WG14_SIGNALS_USER_RAISE_BASE ((DWORD) 0x40000000)

  static DWORD WG14_SIGNALS_PREFIX(win32_exception_code_from_signal)(int c)
  {
    switch(c)
    {
    case SIGABRT:
      return (
      (unsigned long) 0xC0000025L) /*EXCEPTION_NONCONTINUABLE_EXCEPTION*/;
    case SIGBUS:
      return ((unsigned long) 0xC0000006L) /*EXCEPTION_IN_PAGE_ERROR*/;
    case SIGILL:
      return ((unsigned long) 0xC000001DL) /*EXCEPTION_ILLEGAL_INSTRUCTION*/;
    // case signalc::interrupt:
    //  return SIGINT;
    // case signalc::broken_pipe:
    //  return SIGPIPE;
    case SIGSEGV:
      return ((unsigned long) 0xC0000005L) /*EXCEPTION_ACCESS_VIOLATION*/;
    case SIGFPE:
      return ((unsigned long) 0xC0000090L) /*EXCEPTION_FLT_INVALID_OPERATION*/;
    default:
      // No native Win32 code for this signal: use a user-defined code that
      // round-trips through signal_from_win32_exception_code(). The vectored
      // handler then dispatches any decider installed for it, or the
      // software-raise-unclaimed path returns false -- POSIX parity for
      // stdc_raise(SIGINT) etc. (analysis.md 2.12/V4). The signo is masked
      // into the user range's low bits so a negative signo (analysis.md
      // 4.15/Z4) also round-trips: it recovers as a large positive signo that
      // the signo-to-sighandler map's get() bounds-checks and treats as
      // absent, so stdc_raise() returns false instead of aborting or reaching
      // Windows Error Reporting.
      return WG14_SIGNALS_USER_RAISE_BASE | ((DWORD) c & 0x3FFFFFFF);
    }
  }
  static int WG14_SIGNALS_PREFIX(signal_from_win32_exception_code)(DWORD c)
  {
    switch(c)
    {
    case((unsigned long) 0xC0000025L) /*EXCEPTION_NONCONTINUABLE_EXCEPTION*/:
      return SIGABRT;
    case((unsigned long) 0xC0000006L) /*EXCEPTION_IN_PAGE_ERROR*/:
      return SIGBUS;
    case((unsigned long) 0xC000001DL) /*EXCEPTION_ILLEGAL_INSTRUCTION*/:
      return SIGILL;
    // case SIGINT:
    //  return signalc::interrupt;
    // case SIGPIPE:
    //  return signalc::broken_pipe;
    case((unsigned long) 0xC0000005L) /*EXCEPTION_ACCESS_VIOLATION*/:
      return SIGSEGV;
    case((unsigned long) 0xC000008DL) /*EXCEPTION_FLT_DENORMAL_OPERAND*/:
    case((unsigned long) 0xC000008EL) /*EXCEPTION_FLT_DIVIDE_BY_ZERO*/:
    case((unsigned long) 0xC000008FL) /*EXCEPTION_FLT_INEXACT_RESULT*/:
    case((unsigned long) 0xC0000090L) /*EXCEPTION_FLT_INVALID_OPERATION*/:
    case((unsigned long) 0xC0000091L) /*EXCEPTION_FLT_OVERFLOW*/:
    case((unsigned long) 0xC0000092L) /*EXCEPTION_FLT_STACK_CHECK*/:
    case((unsigned long) 0xC0000093L) /*EXCEPTION_FLT_UNDERFLOW*/:
    case((unsigned long) 0xC0000094L) /*EXCEPTION_INT_DIVIDE_BY_ZERO*/:
    case((unsigned long) 0xC0000095L) /*EXCEPTION_INT_OVERFLOW*/:
      return SIGFPE;
    default:
      // Reverse of win32_exception_code_from_signal()'s user-defined mapping:
      // recover the signo for a raise of a signal with no native exception
      // code, and report "not a supported exception code" for anything else.
      // User-defined codes live in 0x40000000-0x7FFFFFFF; all genuine
      // hardware/OS exception codes are >= 0x80000000, so the high-bit check
      // cleanly separates a library raise from a real exception.
      if((c & 0x80000000) == 0 && c >= WG14_SIGNALS_USER_RAISE_BASE)
      {
        return (int) (c - WG14_SIGNALS_USER_RAISE_BASE);
      }
      return 0;
    }
  }

  static void WG14_SIGNALS_PREFIX(prepare_rsi)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi, const int signo,
  EXCEPTION_POINTERS *ptrs)
  {
    memset(rsi, 0, sizeof(*rsi));
    rsi->signo = signo;
    if(ptrs->ExceptionRecord->NumberParameters >= 2 &&
       ptrs->ExceptionRecord
       ->ExceptionInformation[ptrs->ExceptionRecord->NumberParameters - 2] ==
       (ULONG_PTR) 0xdeadbeefdeadbeef)
    {
      rsi->raw_context =
      (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) ptrs->ExceptionRecord
      ->ExceptionInformation[ptrs->ExceptionRecord->NumberParameters - 1];
    }
    else
    {
      rsi->raw_context =
      (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) ptrs->ContextRecord;
    }
    rsi->raw_info =
    (WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) *) ptrs->ExceptionRecord;
    if(ptrs->ExceptionRecord->NumberParameters >= 3)
    {
      rsi->error_code =
      (WG14_SIGNALS_PREFIX(thrd_raised_signal_error_code_t))
      ptrs->ExceptionRecord->ExceptionInformation[2];  // NTSTATUS
    }
    if(ptrs->ExceptionRecord->NumberParameters >= 2)
    {
      rsi->addr = (void *) ptrs->ExceptionRecord->ExceptionInformation[1];
    }
  }

  static long WG14_SIGNALS_PREFIX(win32_exception_filter)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi, const sigset_t *guarded,
  const int signo, WG14_SIGNALS_PREFIX(sig_recover_t) recovery,
  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
  union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value, EXCEPTION_POINTERS *ptrs)
  {
    if(sigismember(guarded, signo))
    {
      WG14_SIGNALS_PREFIX(prepare_rsi)(rsi, signo, ptrs);
      rsi->value = value;
      switch(decider(rsi))
      {
      case WG14_SIGNALS_PREFIX(sig_decision_next_decider):
        break;
      case WG14_SIGNALS_PREFIX(sig_decision_resume_execution):
        return EXCEPTION_CONTINUE_EXECUTION;
      case WG14_SIGNALS_PREFIX(sig_decision_invoke_recovery):
        // No recovery routine: continue the exception search (outer frames,
        // then the unhandled filter / global deciders, then default handling)
        // instead of EXCEPTION_CONTINUE_EXECUTION, which would re-execute the
        // faulting instruction forever (analysis.md 1.7/C1).
        return (recovery != WG14_SIGNALS_NULLPTR) ? EXCEPTION_EXECUTE_HANDLER :
                                                    EXCEPTION_CONTINUE_SEARCH;
      }
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
  WG14_SIGNALS_PREFIX(sigguarded)(const sigset_t *signals,
                                  WG14_SIGNALS_PREFIX(sig_func_t) guarded,
                                  WG14_SIGNALS_PREFIX(sig_recover_t) recovery,
                                  WG14_SIGNALS_PREFIX(sig_decide_t) decider,
                                  union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
                                  value)
  {
    if(signals == WG14_SIGNALS_NULLPTR || guarded == WG14_SIGNALS_NULLPTR ||
       decider == WG14_SIGNALS_NULLPTR)
    {
      abort();
    }
    // sigguarded() must set up the calling thread's per-thread TSS, exactly as
    // the POSIX backend does. A Windows thread whose only library interaction
    // is sigguarded() would otherwise have a NULL per-thread state; a genuine
    // fault then claimed by a global decider makes the vectored exception
    // function dereference tss->front on the NULL state, a crash inside the
    // exception handler (analysis.md 2.19/X3).
    if(0 != WG14_SIGNALS_PREFIX(sig_global_tss_state_init)())
    {
      union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret;
      ret.int_value = -1;
      return ret;
    }
    struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
#ifdef __MINGW32__
#error                                                                         \
"FATAL: Donations of a Mingw suitable alternative to __try ... __except are welcome"
#else
  __try
  {
    return guarded(value);
  }
  __except(WG14_SIGNALS_PREFIX(win32_exception_filter)(
  &rsi, signals,
  WG14_SIGNALS_PREFIX(signal_from_win32_exception_code)(GetExceptionCode()),
  recovery, decider, value, GetExceptionInformation()))
  {
    return recovery(&rsi);
  }
#endif
  }

  // You must NOT do anything async signal unsafe in here!
  bool WG14_SIGNALS_PREFIX(stdc_raise)(
  int signo, WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * info,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context)
  {
    // This isn't async signal safe, but caller may not have called
    // siginstall() so we have no other choice within this
    // library
    if(0 != WG14_SIGNALS_PREFIX(sig_global_tss_state_init)())
    {
      return false;
    }
    if(signo == 0)
    {
      // Caller is doing the non-async safe setup
      return false;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *old =
    tss->front,
                                                                       current;
    memset(&current, 0, sizeof(current));
    current.prev = old;
    tss->front = &current;
    if(setjmp(current.buf) != 0)
    {
      // A global decider claimed the raise and longjmp'd back to us.
      tss->front = old;
      tss->software_raise_in_progress = 0;
      tss->software_raise_unclaimed = 0;
      return true;
    }

    const DWORD win32sehcode =
    WG14_SIGNALS_PREFIX(win32_exception_code_from_signal)(signo);
    // info->ExceptionInformation[0] = 0=read 1=write 8=DEP
    // info->ExceptionInformation[1] = causing address
    // info->ExceptionInformation[2] = NTSTATUS causing exception
    tss->software_raise_in_progress = 1;
    tss->software_raise_unclaimed = 0;
    if(info != WG14_SIGNALS_NULLPTR)
    {
      if(raw_context != WG14_SIGNALS_NULLPTR &&
         info->NumberParameters < EXCEPTION_MAXIMUM_PARAMETERS - 2)
      {
        info->ExceptionInformation[info->NumberParameters++] =
        (ULONG_PTR) 0xdeadbeefdeadbeef;
        info->ExceptionInformation[info->NumberParameters++] =
        (ULONG_PTR) raw_context;
      }
      RaiseException(win32sehcode, info->ExceptionFlags, info->NumberParameters,
                     info->ExceptionInformation);
    }
    else
    {
      RaiseException(win32sehcode, 0, 0, WG14_SIGNALS_NULLPTR);
    }
    // RaiseException() returns when the exception was continued (no handler
    // claimed it). Pop the frame pushed above so tss->front never points at
    // this dead stack frame (analysis.md 1.6).
    tss->front = old;
    const int unclaimed = tss->software_raise_unclaimed;
    tss->software_raise_in_progress = 0;
    tss->software_raise_unclaimed = 0;
    // An unclaimed software raise of a signal with no installed handler or
    // decider returns false instead of letting Windows Error Reporting
    // terminate the process (analysis.md 2.16/W5).
    return (unclaimed) ? false : true;
  }

  static long __stdcall WG14_SIGNALS_PREFIX(win32_vectored_exception_function)(
  EXCEPTION_POINTERS *ptrs)
  {
    const int signo = WG14_SIGNALS_PREFIX(signal_from_win32_exception_code)(
    ptrs->ExceptionRecord->ExceptionCode);
    if(signo == 0)
    {
      // Not a supported exception code
      return EXCEPTION_CONTINUE_SEARCH;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    LOCK(state->lock);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
    it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
    &state->signo_to_sighandler_map, signo);
    if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
    {
      // We don't have a handler installed for that signal. If this exception
      // is one of OUR software raises (stdc_raise() of a signal with no
      // installed handler/decider), continue execution so RaiseException()
      // returns and stdc_raise() reports false, instead of the default
      // unhandled behaviour which invokes Windows Error Reporting and
      // terminates the process (analysis.md 2.16/W5). Genuine faults keep
      // EXCEPTION_CONTINUE_SEARCH.
      UNLOCK(state->lock);
      struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
      WG14_SIGNALS_PREFIX(sig_global_tss_state)();
      if(tss != WG14_SIGNALS_NULLPTR && tss->software_raise_in_progress)
      {
        tss->software_raise_unclaimed = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
      return EXCEPTION_CONTINUE_SEARCH;
    }
    struct WG14_SIGNALS_PREFIX(sighandler_info) *item =
    signo_to_sighandler_map_t_value(it);
    struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
    WG14_SIGNALS_PREFIX(prepare_rsi)(&rsi, signo, ptrs);
    // Take a reference on the container for the duration of the raise so a
    // concurrent siguninstall cannot free it while we are unlocked inside a
    // decider call (analysis.md 2.2/W4).
    item->lifetime_refcount++;
    if(item->global_handler.front != WG14_SIGNALS_NULLPTR)
    {
      struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *current =
      item->global_handler.front;
      do
      {
        rsi.value = current->value;
        current->refcount++;
        UNLOCK(state->lock);
        const enum WG14_SIGNALS_PREFIX(sig_decision_t) res =
        current->decider(&rsi);
        LOCK(state->lock);
        if(0 == --current->refcount)
        {
          // Add to free later list
          struct WG14_SIGNALS_PREFIX(global_signal_decider_t) *to_free_later =
          current;
          current = current->next;
          LIST_REMOVE(item->global_handler, to_free_later);
          LIST_INSERT_BACK(item->deferred_frees, to_free_later);
        }
        else
        {
          current = current->next;
        }
        if(res)
        {
          WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
          UNLOCK(state->lock);
          struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
          WG14_SIGNALS_PREFIX(sig_global_tss_state)();
          // If there is a most recent thread local handler, resume there
          // instead. tss may be NULL: the per-thread state is created only by
          // sig_global_tss_state_init() (a prior sigguarded()/stdc_raise() on
          // this thread), and the vectored handler never initialises it, so a
          // genuine fault on a thread that has only ever called siginstall()
          // (or nothing) would otherwise NULL-deref tss->front inside the
          // exception handler (analysis.md 2.10/V2). With no frame to resume,
          // fall through to the "generally end the process" path below.
          if(tss != WG14_SIGNALS_NULLPTR && tss->front != WG14_SIGNALS_NULLPTR)
          {
            longjmp(tss->front->buf, 1);
          }
          // This will generally end the process
          return EXCEPTION_CONTINUE_EXECUTION;
        }
      } while(current != WG14_SIGNALS_NULLPTR);
    }
    // None of our deciders want this, so call previously installed signal
    // handler
    WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
    UNLOCK(state->lock);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  /* The interaction between AddVectoredContinueHandler,
  AddVectoredExceptionHandler, UnhandledExceptionFilter, and frame-based EH is
  completely undocumented in Microsoft documentation. The following is the
  truth, as determined by empirical testing:

  1. Vectored exception handlers get called first, before anything else,
  including frame-based EH. This is not what the MSDN documentation hints at.

  2. Frame-based EH filters are now run.

  3. UnhandledExceptionFilter() is now called. On older Windows, this invokes
  the debugger if being run under the debugger, otherwise continues search. But
  as of at least Windows 7 onwards, if no debugger is attached, it invokes
  Windows Error Reporting to send a core dump to Microsoft.

  4. Vectored continue handlers now get called, AFTER the frame-based EH. Again,
  not what MSDN hints at.


  The change in the default non-debugger behaviour of UnhandledExceptionFilter()
  effectively makes vectored continue handlers useless. I suspect whomever made
  the change at Microsoft didn't realise that vectored continue handlers are
  invoked AFTER the unhandled exception filter, because that's really
  non-obvious from the documentation.

  Anyway this is why we install for both the continue handler and the unhandled
  exception filters. The unhandled exception filter will be called when not
  running under a debugger. The vectored continue handler will be called when
  running under a debugger, as the UnhandledExceptionFilter() function never
  calls the installed unhandled exception filter function if under a debugger.
  */

  static bool WG14_SIGNALS_PREFIX(install_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    (void) item;
    (void) signo;
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    if(0 == state->sighandlers_count)
    {
      state->vectored_continue_handler = AddVectoredContinueHandler(
      true, WG14_SIGNALS_PREFIX(win32_vectored_exception_function));
      if(state->vectored_continue_handler == WG14_SIGNALS_NULLPTR)
      {
        return false;
      }
      state->old_unhandled_exception_filter = SetUnhandledExceptionFilter(
      WG14_SIGNALS_PREFIX(win32_vectored_exception_function));
    }
    return true;
  }
  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    (void) item;
    (void) signo;
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    if(0 == state->sighandlers_count)
    {
      SetUnhandledExceptionFilter(state->old_unhandled_exception_filter);
      RemoveVectoredContinueHandler(state->vectored_continue_handler);
    }
    return true;
  }

#ifdef _MSC_VER
#pragma warning(pop)
#endif
#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif


#ifdef __cplusplus
}
#endif

#endif
