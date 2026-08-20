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

struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t)
{
  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) * prev;
  bool exception_was_unclaimed;

  DWORD ExceptionCode;
  DWORD ExceptionFlags;
  DWORD NumberParameters;
  ULONG_PTR ExceptionInformationFirst;
};

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
    case((unsigned long) 0xC00000FDL) /*EXCEPTION_STACK_OVERFLOW*/:
      // EXCEPTION_STACK_OVERFLOW (a genuine stack overflow / exhausted guard
      // page) maps to SIGSEGV exactly as on POSIX, where a stack overflow
      // delivers SIGSEGV (analysis.md 3.18/X4). Without this case the fault
      // returned signo == 0 -> EXCEPTION_CONTINUE_SEARCH and Windows Error
      // Reporting terminated the process with no library involvement. Note a
      // decider that recovers from a stack overflow is responsible for
      // _resetstkoflw() to restore the guard page before continuing.
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
      (WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t))
      ptrs->ExceptionRecord->ExceptionInformation[2];  // NTSTATUS
    }
    if(ptrs->ExceptionRecord->NumberParameters >= 2)
    {
      rsi->addr = (void *) ptrs->ExceptionRecord->ExceptionInformation[1];
    }
  }

  // The per-guard state of win32_exception_filter. Only the exception context
  // itself is passed separately: GetExceptionInformation()/GetExceptionCode()
  // are valid solely in the sigguarded() filter expression (the filter function
  // cannot call them, per the Win32 docs), so the expression passes the former
  // and the filter derives signo from ptrs->ExceptionRecord->ExceptionCode (as
  // win32_global_decider_pass does). The remaining state travels in this
  // object, which sigguarded() builds once at guard entry and passes by
  // pointer to the filter. It is mutable rather than const: the filter fills
  // rsi (prepare_rsi) and, on the recovery claim path, restores the per-thread
  // chain via tss.
  struct WG14_SIGNALS_PREFIX(win32_exception_filter_state)
  {
    struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) * tss;
    const sigset_t *guarded;
    // sig_recover_t/sig_decide_t are function types (thrd_signal_handle.h), so
    // the members are pointers to them: a struct member of function type is
    // ill-formed in both C11 and C++ (MSVC C2032/C3867).
    WG14_SIGNALS_PREFIX(sig_recover_t) * recovery;
    WG14_SIGNALS_PREFIX(sig_decide_t) * decider;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value;
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *
    tss_front_at_entry;
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) * win_at_entry;
  };

  static long WG14_SIGNALS_PREFIX(win32_exception_filter)(
  struct WG14_SIGNALS_PREFIX(win32_exception_filter_state) * state,
  EXCEPTION_POINTERS *ptrs)
  {
    const int signo = WG14_SIGNALS_PREFIX(signal_from_win32_exception_code)(
    ptrs->ExceptionRecord->ExceptionCode);
    // sigismember() returns -1 for signo outside 1..32; only a genuine 1
    // (member) may invoke the decider, so exceptions that are not signals
    // (C++ /EHa, unmapped fault codes, user-range raises) skip the frame
    // decider instead of running it with signo == 0 or a bogus signo
    // (analysis.md SIGM).
    if(sigismember(state->guarded, signo) == 1)
    {
      WG14_SIGNALS_PREFIX(prepare_rsi)(&state->rsi, signo, ptrs);
      state->rsi.value = state->value;
      state->rsi.internal_local_decider = state->tss->front;
      switch(state->decider(&state->rsi))
      {
      case WG14_SIGNALS_PREFIX(sig_decision_next_decider):
        break;
      case WG14_SIGNALS_PREFIX(sig_decision_resume_execution):
        return EXCEPTION_CONTINUE_EXECUTION;
      case WG14_SIGNALS_PREFIX(sig_decision_call_recovery):
        // No recovery routine: continue the exception search (outer frames,
        // then the unhandled filter / global deciders, then default handling)
        // instead of EXCEPTION_CONTINUE_EXECUTION, which would re-execute the
        // faulting instruction forever (analysis.md 1.7/C1).
        if(state->recovery == WG14_SIGNALS_NULLPTR)
        {
          return EXCEPTION_CONTINUE_SEARCH;
        }
        // Returning EXECUTE_HANDLER makes SEH unwind the stack below this
        // __except, discarding every frame stdc_raise() pushed onto the
        // per-thread chain for a raise initiated inside this guard (its own
        // pop paths never run because RaiseException() does not return).
        // Frames pushed after sigguarded() entry are all below this __except
        // and are discarded, so restore the chain heads captured at entry:
        // nulling the whole chain would instead drop outer in-flight raise
        // frames above this __except that survive the unwind, leaving a
        // later global decider longjmping into a NULL frame (analysis.md
        // RFLK).
        state->tss->front = state->tss_front_at_entry;
        state->tss->stdc_raise_initiated_exception = state->win_at_entry;
        return EXCEPTION_EXECUTE_HANDLER;
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
      return WG14_SIGNALS_PREFIX(SIGGUARDED_FAILURE_VALUE);
    }
    // Snapshot the per-thread chain heads so the frame filter can restore them
    // when a claimed raise unwinds the stack below this __except. Frames pushed
    // after this point live inside this guard (below the __except) and are
    // discarded by that unwind; frames pushed before it (outer in-flight
    // raises) survive and must be kept (analysis.md RFLK). rsi and tss live in
    // the state object so the filter fills the former and the __except block
    // reads it back via the same object.
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    struct WG14_SIGNALS_PREFIX(win32_exception_filter_state) state = {
    {0},     tss,   signals,    recovery,
    decider, value, tss->front, tss->stdc_raise_initiated_exception,
    };
#ifdef __MINGW32__
#error                                                                         \
"FATAL: Donations of a Mingw suitable alternative to __try ... __except are welcome"
#else
  __try
  {
    return guarded(value);
  }
  __except(WG14_SIGNALS_PREFIX(win32_exception_filter)(
  &state, GetExceptionInformation()))
  {
    return recovery(&state.rsi);
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
      // The per-thread TSS setup failed (e.g. OOM). The return value alone
      // cannot distinguish this from "no decider installed for this signal"
      // (both are false), so report the failure via errno: the init chain
      // already sets errno on its own failure paths (calloc -> ENOMEM,
      // tss_async_signal_safe_thread_init -> ENOMEM/EINVAL), and we guarantee
      // a diagnostic even if a lower layer forgot (plans/analysis.md 3.7).
      // In particular the documented setup call stdc_raise(0, NULL, NULL)
      // then lets the caller detect that setup actually failed.
      if(errno == 0)
      {
        errno = ENOMEM;
      }
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
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) current2;
    memset(&current2, 0, sizeof(current2));
    current2.prev = tss->stdc_raise_initiated_exception;
    if(setjmp(current.buf) != 0)
    {
      // A global decider claimed the raise and longjmp'd back to us.
      tss->front = old;
      tss->stdc_raise_initiated_exception = current2.prev;
      return true;
    }
    tss->front = &current;
    tss->stdc_raise_initiated_exception = &current2;
    const DWORD win32sehcode =
    WG14_SIGNALS_PREFIX(win32_exception_code_from_signal)(signo);
    // info->ExceptionInformation[0] = 0=read 1=write 8=DEP
    // info->ExceptionInformation[1] = causing address
    // info->ExceptionInformatio n[2] = NTSTATUS causing exception
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
      // Put the vectored exception handler into "we raised this exception"
      // mode. This means it kicks back to us instead of invoking the Windows
      // handler if it was unhandled.
      current2.ExceptionCode = win32sehcode;
      current2.ExceptionFlags = info->ExceptionFlags;
      current2.NumberParameters = info->NumberParameters;
      current2.ExceptionInformationFirst = info->ExceptionInformation[0];
      RaiseException(win32sehcode, info->ExceptionFlags, info->NumberParameters,
                     info->ExceptionInformation);
    }
    else
    {
      current2.ExceptionCode = win32sehcode;
      RaiseException(win32sehcode, 0, 0, WG14_SIGNALS_NULLPTR);
    }
    // RaiseException() returns when the exception was continued (no handler
    // claimed it). Pop the frame pushed above so tss->front never points at
    // this dead stack frame (analysis.md 1.6).
    tss->front = old;
    tss->stdc_raise_initiated_exception = current2.prev;
    // An unclaimed software raise of a signal with no installed handler or
    // decider returns false instead of letting Windows Error Reporting
    // terminate the process (analysis.md 2.16/W5).
    return (current2.exception_was_unclaimed) ? false : true;
  }

  // You must NOT do anything async signal unsafe in here!
  void WG14_SIGNALS_PREFIX(sigdecider_abandon)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
  {
    if(!rsi->internal_decider_is_abandoned)
    {
      struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
      WG14_SIGNALS_PREFIX(sig_global_tss_state)();
      if(rsi->internal_local_decider != WG14_SIGNALS_NULLPTR)
      {
        if(tss->front != rsi->internal_local_decider)
        {
          // sigdecider_abandon not called on topmost sigguarded()
          assert(tss->front == rsi->internal_local_decider);
          abort();
        }
        // Pop the top most sigguarded()
        tss->front = tss->front->prev;
      }
      if(tss->stdc_raise_initiated_exception != WG14_SIGNALS_NULLPTR)
      {
        rsi->internal_win_state = tss->stdc_raise_initiated_exception;
        tss->stdc_raise_initiated_exception =
        tss->stdc_raise_initiated_exception->prev;
      }
      if(rsi->internal_sighandler != WG14_SIGNALS_NULLPTR ||
         rsi->internal_global_decider != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
        WG14_SIGNALS_PREFIX(sig_global_state)();
        LOCK(state->lock);
        if(rsi->internal_sighandler != WG14_SIGNALS_NULLPTR)
        {
          rsi->internal_sighandler->lifetime_refcount--;
        }
        if(rsi->internal_global_decider != WG14_SIGNALS_NULLPTR)
        {
          rsi->internal_global_decider->refcount--;
        }
        UNLOCK(state->lock);
        // Pop the top most sigguarded()
        if(tss->front != WG14_SIGNALS_NULLPTR)
        {
          rsi->internal_local_decider = tss->front;
          tss->front = tss->front->prev;
        }
      }
      rsi->internal_decider_is_abandoned = true;
    }
  }

  // You must NOT do anything async signal unsafe in here!
  void WG14_SIGNALS_PREFIX(sigdecider_abandon_resume)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
  {
    if(rsi->internal_decider_is_abandoned)
    {
      struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
      WG14_SIGNALS_PREFIX(sig_global_tss_state)();
      if(rsi->internal_win_state != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) *newstate =
        (struct WG14_SIGNALS_PREFIX(
        sig_global_state_tss_state_win_t) *) rsi->internal_win_state;
        if(newstate->prev != tss->stdc_raise_initiated_exception)
        {
          // sigdecider_abandon not called on topmost sigguarded()
          assert(newstate->prev == tss->stdc_raise_initiated_exception);
          abort();
        }
        tss->stdc_raise_initiated_exception = newstate;
      }
      if(rsi->internal_local_decider != WG14_SIGNALS_NULLPTR)
      {
        if(tss->front != rsi->internal_local_decider->prev)
        {
          // sigdecider_abandon not called on topmost sigguarded()
          assert(tss->front == rsi->internal_local_decider->prev);
          abort();
        }
        tss->front = rsi->internal_local_decider;
      }
      if(rsi->internal_sighandler != WG14_SIGNALS_NULLPTR ||
         rsi->internal_global_decider != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
        WG14_SIGNALS_PREFIX(sig_global_state)();
        LOCK(state->lock);
        if(rsi->internal_sighandler != WG14_SIGNALS_NULLPTR)
        {
          rsi->internal_sighandler->lifetime_refcount++;
        }
        if(rsi->internal_global_decider != WG14_SIGNALS_NULLPTR)
        {
          rsi->internal_global_decider->refcount++;
        }
        UNLOCK(state->lock);
      }
      rsi->internal_decider_is_abandoned = false;
    }
  }


  // The vectored exception function is split into two registered entry points
  // (install_sighandler_impl below): the unhandled exception filter runs the
  // global-decider pass on the no-debugger path, and the vectored continue
  // handler is what runs over a debugger (the OS does not call the installed
  // filter there). On the no-debugger path Windows invokes the continue handler
  // AFTER the unhandled filter for the SAME exception, so without a marker the
  // global deciders would run twice per exception — a double invocation of
  // every side-effecting decider (analysis.md 3.15/V5). The filter therefore
  // snapshots the identity fields of the EXCEPTION_RECORD the global-decider
  // pass last ran for, and the decision it produced; the continue handler
  // reuses the recorded decision for the same exception instead of running the
  // deciders again, then consumes the entry (analysis VDED) so a later
  // exception can never silently skip the pass. Per-thread: exception dispatch
  // is per-thread, each dispatch has its own EXCEPTION_RECORD (so a nested
  // exception raised by a decider has a different record and runs the pass
  // fresh), and under a debugger only the continue handler runs so the pass
  // executes exactly once.
  static WG14_SIGNALS_THREAD_LOCAL struct WG14_SIGNALS_PREFIX(
  sig_global_state_tss_state_win_t) wg14_last_global_decider_record;
  static WG14_SIGNALS_THREAD_LOCAL LONG wg14_last_global_decider_result;

  // Compares a sig_global_state_tss_state_win_t snapshot (a raise-initiated-
  // exception frame or the V5 dedup entry above) against a live
  // EXCEPTION_RECORD, exactly as the raise detection in
  // win32_global_decider_pass does: the two describe the same exception when
  // the exception code, flags, parameter count and (when parameters are
  // present) the first parameter all agree (analysis.md 3.15/V5). Note the
  // first-parameter comparison applies only when parameters exist: a
  // 0-parameter exception -- stdc_raise(signo, NULL, NULL) and RaiseException
  // with no arguments, the common paths -- must match on code/flags/count
  // alone (the pre-2026-08-20 form `record->NumberParameters > 0 && ...` made
  // every 0-parameter match fail, breaking both the V5 dedup and the
  // unclaimed-raise detection on Windows CI).
  static bool WG14_SIGNALS_PREFIX(win32_exception_record_matches)(
  const struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_win_t) * state,
  const EXCEPTION_RECORD *record)
  {
    return state->ExceptionCode == record->ExceptionCode &&
           state->ExceptionFlags == record->ExceptionFlags &&
           state->NumberParameters == record->NumberParameters &&
           (record->NumberParameters == 0 || state->ExceptionInformationFirst ==
                                             record->ExceptionInformation[0]);
  }

  // Records the V5 dedup decision for an exception so the follow-up vectored
  // continue handler reuses it instead of re-running the pass (analysis.md
  // 3.15/V5).
  static void WG14_SIGNALS_PREFIX(win32_record_global_decider_decision)(
  const EXCEPTION_RECORD *record, const long result)
  {
    wg14_last_global_decider_record.ExceptionCode = record->ExceptionCode;
    wg14_last_global_decider_record.ExceptionFlags = record->ExceptionFlags;
    wg14_last_global_decider_record.NumberParameters = record->NumberParameters;
    wg14_last_global_decider_record.ExceptionInformationFirst =
    (record->NumberParameters > 0) ? record->ExceptionInformation[0] : 0;
    wg14_last_global_decider_result = result;
  }

  // Detects whether the current exception is an unclaimed software raise -- a
  // stdc_raise() with no map entry, or with a map entry that no decider
  // claimed. If so, marks the in-flight raise frame unclaimed so stdc_raise()
  // returns false instead of the exception reaching Windows Error Reporting
  // (analysis.md 2.16/W5); returns whether it was one.
  static bool WG14_SIGNALS_PREFIX(win32_unclaimed_software_raise)(
  const EXCEPTION_RECORD *record)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    if(tss != WG14_SIGNALS_NULLPTR &&
       tss->stdc_raise_initiated_exception != WG14_SIGNALS_NULLPTR &&
       WG14_SIGNALS_PREFIX(win32_exception_record_matches)(
       tss->stdc_raise_initiated_exception, record))
    {
      tss->stdc_raise_initiated_exception->exception_was_unclaimed = true;
      return true;
    }
    return false;
  }

  // Runs the global-decider pass for one exception dispatch and, when the pass
  // returns a disposition whose record the follow-up vectored continue handler
  // must reuse, records the decision (analysis.md 3.15/V5).
  static long __stdcall
  WG14_SIGNALS_PREFIX(win32_global_decider_pass)(EXCEPTION_POINTERS *ptrs)
  {
    const EXCEPTION_RECORD *record = ptrs->ExceptionRecord;
    const int signo = WG14_SIGNALS_PREFIX(signal_from_win32_exception_code)(
    record->ExceptionCode);
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
      UNLOCK(state->lock);
      // We don't have a handler installed for that signal. If this exception
      // is one of OUR software raises (stdc_raise() of a signal with no
      // installed handler/decider), continue execution so RaiseException()
      // returns and stdc_raise() reports false, instead of the default
      // unhandled behaviour which invokes Windows Error Reporting and
      // terminates the process (analysis.md 2.16/W5). Genuine faults keep
      // EXCEPTION_CONTINUE_SEARCH.
      return WG14_SIGNALS_PREFIX(win32_unclaimed_software_raise)(record) ?
             EXCEPTION_CONTINUE_EXECUTION :
             EXCEPTION_CONTINUE_SEARCH;
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
        // In case they wish to abandon
        rsi.internal_sighandler = item;
        rsi.internal_global_decider = current;
        const enum WG14_SIGNALS_PREFIX(sig_decision) res =
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
          // This will generally end the process. Record the decision so the
          // vectored continue handler's follow-up invocation for the same
          // exception reuses it instead of re-running the deciders
          // (analysis.md 3.15/V5).
          WG14_SIGNALS_PREFIX(win32_record_global_decider_decision)(
          record, EXCEPTION_CONTINUE_EXECUTION);
          return EXCEPTION_CONTINUE_EXECUTION;
        }
      } while(current != WG14_SIGNALS_NULLPTR);
    }
    // None of our deciders want this. A software raise (stdc_raise()) that no
    // global decider claimed must return false to the caller, not reach WER:
    // the installed map entry means only that a handler is registered for the
    // signal, not that the raise should terminate the process when unclaimed
    // (analysis.md 2.16/W5 -- the map-entry-no-decider arm of PREI). Check the
    // raise-initiated frame before falling back to "call previously installed
    // signal handler" below.
    if(WG14_SIGNALS_PREFIX(win32_unclaimed_software_raise)(record))
    {
      WG14_SIGNALS_PREFIX(win32_record_global_decider_decision)(
      record, EXCEPTION_CONTINUE_EXECUTION);
      WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
      UNLOCK(state->lock);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Not a software raise: call previously installed signal handler. Record
    // the decision for the V5 dedup (analysis.md 3.15).
    WG14_SIGNALS_PREFIX(win32_record_global_decider_decision)(
    record, EXCEPTION_CONTINUE_SEARCH);
    WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
    UNLOCK(state->lock);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Registered via SetUnhandledExceptionFilter(). On the no-debugger path this
  // is the first invocation for an exception, so it always resolves the global-
  // decider pass fresh; the pass's recorded decision is what the vectored
  // continue handler reuses. Clear the entry first so a preceding pass that did
  // not record a decision (an unclaimed software raise, or a decider that
  // abandoned via longjmp) can never leave a previous exception's identity to
  // be spuriously reused by a later exception (analysis VDED).
  static long __stdcall WG14_SIGNALS_PREFIX(win32_unhandled_exception_filter)(
  EXCEPTION_POINTERS *ptrs)
  {
    memset(&wg14_last_global_decider_record, 0,
           sizeof(wg14_last_global_decider_record));
    return WG14_SIGNALS_PREFIX(win32_global_decider_pass)(ptrs);
  }

  // Registered via AddVectoredContinueHandler(). Under a debugger this is the
  // only invocation for an exception, so when no decision was recorded for the
  // current exception identity it resolves the global-decider pass fresh and
  // leaves nothing cached. On the no-debugger path it is the second invocation
  // for the same exception, right after the unhandled exception filter above:
  // return the recorded decision and consume the entry (analysis VDED) so a
  // later exception cannot silently skip the pass.
  static long __stdcall
  WG14_SIGNALS_PREFIX(win32_vectored_continue_handler)(EXCEPTION_POINTERS *ptrs)
  {
    const EXCEPTION_RECORD *record = ptrs->ExceptionRecord;
    if(WG14_SIGNALS_PREFIX(win32_exception_record_matches)(
       &wg14_last_global_decider_record, record))
    {
      memset(&wg14_last_global_decider_record, 0,
             sizeof(wg14_last_global_decider_record));
      return wg14_last_global_decider_result;
    }
    const long result = WG14_SIGNALS_PREFIX(win32_global_decider_pass)(ptrs);
    memset(&wg14_last_global_decider_record, 0,
           sizeof(wg14_last_global_decider_record));
    return result;
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
      true, WG14_SIGNALS_PREFIX(win32_vectored_continue_handler));
      if(state->vectored_continue_handler == WG14_SIGNALS_NULLPTR)
      {
        return false;
      }
      state->old_unhandled_exception_filter = SetUnhandledExceptionFilter(
      WG14_SIGNALS_PREFIX(win32_unhandled_exception_filter));
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
