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

#ifndef WG14_SIGNALS_THRD_SIGNAL_HANDLE_POSIX_IPP
#define WG14_SIGNALS_THRD_SIGNAL_HANDLE_POSIX_IPP

// A wrong-platform include is a clear compile error instead of a silent
// mis-compile (plans/ideas.md 4.4).
#ifdef _WIN32
#error "thrd_signal_handle_posix.c.ipp must only be included on non-Windows"
#endif

#include "../../thrd_signal_handle.h"

#include <pthread.h>
#include <signal.h>

#if !defined(__APPLE__)
// getcontext(), for capturing the calling context as the implementation-defined
// default raw_context (N3924 7.14.2.9 p8). Not on macOS: its <ucontext.h>
// hard-errors without _XOPEN_SOURCE and its getcontext() is deprecated and
// broken on arm64, so the calling frame is captured directly there instead.
#include <ucontext.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>  // SYS_gettid, SYS_rt_tgsigqueueinfo
#include <unistd.h>       // syscall(), getpid()
#endif

#if defined(__linux__)
//! \brief Delivers `signo` to the calling thread with `value` carried in the
//! delivered siginfo's `si_value.sival_ptr`, so the raw handler can match the
//! delivery to the stdc_raise() record that produced it. This implements the
//! Linux default of `WG14_SIGNALS_RAISE_WITH_TOKEN()` (config.h) via the
//! thread-directed `rt_tgsigqueueinfo(2)` syscall -- not `pthread_sigqueue()`,
//! which glibc declares only under `_GNU_SOURCE`, a macro this library must not
//! define in a public header. The kernel fills in `si_pid`/`si_uid` for
//! `SI_QUEUE`; only `si_value` is ours. Returns 0 on success, -1 on failure
//! (like `WG14_SIGNALS_KILL_SELF()`). `static inline` so that an embedding libc
//! which overrides `WG14_SIGNALS_RAISE_WITH_TOKEN()` does not leave it unused;
//! on a Linux target without `rt_tgsigqueueinfo` it degrades to a tokenless
//! self-delivery.
static inline int
WG14_SIGNALS_PREFIX(raise_with_token)(const int signo, const union sigval value)
{
#if defined(SYS_rt_tgsigqueueinfo)
  siginfo_t si;
  WG14_SIGNALS_MEMSET(&si, 0, sizeof(si));
  si.si_signo = signo;
  si.si_code = SI_QUEUE;
  si.si_value = value;
  return (int) syscall(SYS_rt_tgsigqueueinfo, getpid(),
                       (pid_t) WG14_SIGNALS_GETTID(), signo, &si);
#else
  (void) value;
  return WG14_SIGNALS_KILL_SELF(signo);
#endif
}
#endif

#include "thrd_signal_handle_common.ipp.ipp"

#include "linked_list.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef WG14_SIGNALS_SETJMP
#if WG14_SIGNALS_HAVE__SETJMP
#define WG14_SIGNALS_SETJMP _setjmp
#else
#define WG14_SIGNALS_SETJMP setjmp
#endif
#endif
#ifndef WG14_SIGNALS_LONGJMP
#if WG14_SIGNALS_HAVE__SETJMP
#define WG14_SIGNALS_LONGJMP _longjmp
#else
#define WG14_SIGNALS_LONGJMP longjmp
#endif
#endif

#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
#endif

  static __attribute__((constructor))
  const sigset_t *WG14_SIGNALS_PREFIX(synchronous_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {
    SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGPIPE, SIGSEGV,
#ifdef SIGSYS
    SIGSYS,
#endif
    };
    if(WG14_SIGNALS_SIGISMEMBER(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    WG14_SIGNALS_SIGEMPTYSET(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      WG14_SIGNALS_SIGADDSET(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_synchronous)(sigset_t *set)
  {
    WG14_SIGNALS_MEMCPY(set, WG14_SIGNALS_PREFIX(synchronous_sigset)(),
                        sizeof(*set));
    return 0;
  }

  static __attribute__((constructor)) const sigset_t *
  WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {SIGALRM, SIGCHLD, SIGCONT,  SIGHUP,  SIGINT,
                                 SIGKILL, SIGSTOP, SIGTERM,  SIGTSTP, SIGTTIN,
                                 SIGTTOU, SIGUSR1, SIGUSR2,
#ifdef SIGPOLL
                                 SIGPOLL,
#endif
                                 SIGPROF, SIGURG,  SIGVTALRM};
    if(WG14_SIGNALS_SIGISMEMBER(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    WG14_SIGNALS_SIGEMPTYSET(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      WG14_SIGNALS_SIGADDSET(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_nondebug)(sigset_t *set)
  {
    WG14_SIGNALS_MEMCPY(
    set, WG14_SIGNALS_PREFIX(asynchronous_nondebug_sigset)(), sizeof(*set));
    return 0;
  }

  static __attribute__((constructor)) const sigset_t *
  WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(void)
  {
    static sigset_t v;
    static const int signos[] = {
    SIGQUIT,
    SIGTRAP,
#ifdef SIGXCPU
    SIGXCPU,
#endif
#ifdef SIGXFSZ
    SIGXFSZ,
#endif
    };
    if(WG14_SIGNALS_SIGISMEMBER(&v, signos[0]))
    {
      return &v;
    }
    sigset_t x;
    WG14_SIGNALS_SIGEMPTYSET(&x);
    for(size_t n = 0; n < sizeof(signos) / sizeof(signos[0]); n++)
    {
      WG14_SIGNALS_SIGADDSET(&x, signos[n]);
    }
    v = x;
    return &v;
  }
  int WG14_SIGNALS_PREFIX(sigfillset_asynchronous_debug)(sigset_t *set)
  {
    WG14_SIGNALS_MEMCPY(set, WG14_SIGNALS_PREFIX(asynchronous_debug_sigset)(),
                        sizeof(*set));
    return 0;
  }

#if 0
// Reset the SIGABRT handler, and call abort()
static void __attribute__((noreturn)) default_abort(void)
{
  struct sigaction sa;
  WG14_SIGNALS_MEMSET(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  (void) WG14_SIGNALS_SIGACTION(SIGABRT, &sa, WG14_SIGNALS_NULLPTR);
  WG14_SIGNALS_ABORT();
}
#endif


  // The library's built-in default action for a signal whose previously
  // installed disposition was SIG_DFL: reset the kernel disposition to SIG_DFL
  // and re-deliver the signal to this thread. This is the fallback used when
  // no callback is installed with siginstall_set_default_action_np() (the
  // former WG14_SIGNALS_DEFAULT_ACTION macro expansion).
  static void WG14_SIGNALS_PREFIX(default_action_impl)(const int signo,
                                                       siginfo_t *siginfo,
                                                       void *context)
  {
    (void) siginfo;
    (void) context;
    struct sigaction dfl;
    WG14_SIGNALS_MEMSET(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    (void) WG14_SIGNALS_SIGACTION(signo, &dfl, WG14_SIGNALS_NULLPTR);
    (void) WG14_SIGNALS_KILL_SELF(signo);
  }

  // Invoke a sigaction as if it were the first signal handler
  static bool WG14_SIGNALS_PREFIX(invoke_sigaction)(const struct sigaction *sa,
                                                    const int signo,
                                                    siginfo_t *siginfo,
                                                    void *context)
  {
    if((sa->sa_flags & SA_SIGINFO) != 0)
    {
      if(siginfo == WG14_SIGNALS_NULLPTR)
      {
        // stdc_raise(signo, NULL, NULL) hands off to the previously installed
        // handler with no OS info (analysis.md NSIH). A real delivery always
        // carries a valid siginfo_t, and a SA_SIGINFO handler may read
        // si->si_signo / si->si_code without checking for NULL, so synthesise a
        // minimal zeroed siginfo_t instead of passing NULL through.
        siginfo_t synthesized;
        WG14_SIGNALS_MEMSET(&synthesized, 0, sizeof(synthesized));
        synthesized.si_signo = signo;
        synthesized.si_code = SI_USER;
        sa->sa_sigaction(signo, &synthesized, context);
      }
      else
      {
        sa->sa_sigaction(signo, siginfo, context);
      }
      return true;
    }
    if(sa->sa_handler == SIG_DFL)
    {
      switch(signo)
      {
      case SIGCHLD:
      case SIGURG:
#ifdef SIGWINCH
      case SIGWINCH:
#endif
        // The default is to ignore
        return false;
      default:
      {
        // The default is to terminate (possibly with a core dump), stop, or
        // continue. Take it by resetting to SIG_DFL and re-delivering the
        // signal to this thread. Save the current (library) handler first and
        // restore it afterwards: the restore is dead code when a terminating
        // default kills the process in the delivery, but for a stop/continue
        // default (SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU/SIGCONT) the process
        // survives the stop, and leaving the kernel handler reset to SIG_DFL
        // would make later deliveries of that signal bypass the library until
        // a re-install (analysis DFLT). The stop happens during the signal
        // delivery that completes this thread's re-raise, so the restore below
        // runs only after the process has been resumed. The default action is
        // taken by the callback installed with
        // siginstall_set_default_action_np() when one is set, else by the
        // library's built-in reset-and-re-raise above (an embedder sets the
        // callback to re-deliver with the original `siginfo` preserved, for
        // core-dump fidelity). The callback is read with an atomic load
        // because this function runs in async-signal-safe context (the raw
        // signal handler) where the global spinlock must not be taken.
        struct sigaction current;
        WG14_SIGNALS_MEMSET(&current, 0, sizeof(current));
        (void) WG14_SIGNALS_SIGACTION(signo, WG14_SIGNALS_NULLPTR, &current);
        struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
        WG14_SIGNALS_PREFIX(sig_global_state)();
        WG14_SIGNALS_PREFIX(sig_default_action_np_t) *default_action =
        atomic_load_explicit(&state->default_action,
                             WG14_SIGNALS_ATOMIC_PREFIX memory_order_acquire);
        if(default_action != WG14_SIGNALS_NULLPTR)
        {
          default_action(signo, siginfo, context);
        }
        else
        {
          WG14_SIGNALS_PREFIX(default_action_impl)(signo, siginfo, context);
        }
        (void) WG14_SIGNALS_SIGACTION(signo, &current, WG14_SIGNALS_NULLPTR);
        return true;
      }
      }
    }
    if(sa->sa_handler == SIG_IGN)
    {
      // We do nothing
      return false;
    }
    sa->sa_handler(signo);
    return true;
  }

  static void WG14_SIGNALS_PREFIX(prepare_rsi)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi, const int signo,
  WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * siginfo,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * context)
  {
    rsi->signo = signo;
    rsi->raw_context = context;
    if(siginfo != WG14_SIGNALS_NULLPTR)
    {
      rsi->raw_info = siginfo;
      rsi->error_code = siginfo->si_errno;
      rsi->addr = siginfo->si_addr;
    }
    else
    {
      // stdc_raise(signo, NULL, NULL) must hand deciders deterministic
      // "no OS info" values, not indeterminate garbage or a stale pointer
      // from an earlier raise in the same frame (analysis.md 2.14/W2). Do
      // NOT memset the whole struct here: on the frame path `value` is
      // pre-set by sigguarded() and must survive.
      rsi->raw_info = WG14_SIGNALS_NULLPTR;
      rsi->error_code = 0;
      rsi->addr = WG14_SIGNALS_NULLPTR;
    }
    rsi->internal_decider_is_abandoned = false;
    rsi->internal_global_decider = WG14_SIGNALS_NULLPTR;
    rsi->internal_local_decider = WG14_SIGNALS_NULLPTR;
    rsi->internal_sighandler = WG14_SIGNALS_NULLPTR;
  }

  // Releases the calling thread's in-flight stdc_raise() records that were
  // created after `seq_at_entry`, the per-thread stdc_raise_seq value
  // snapshotted when a guard frame was entered. A non-local jump out of a
  // delivery (recovery, or an abandoned decider) unwinds the stdc_raise() calls
  // made inside the guard, whether or not the raw handler had already consumed
  // their records; their raw_info/raw_context point into the unwound stack, so
  // they must not be matched by a later delivery. Records are pushed with
  // increasing seq and removed LIFO, so the unwound ones are exactly the head
  // prefix with a newer seq; an outer raise that was already in flight when the
  // frame was entered has an older seq and survives, which is what makes "jump
  // back to the earlier raise, then clean up" correct.
  static void WG14_SIGNALS_PREFIX(release_stdc_raise_records_after)(
  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) * tss,
  const uintptr_t seq_at_entry)
  {
    while(tss->stdc_raise_records != WG14_SIGNALS_NULLPTR &&
          tss->stdc_raise_records->seq > seq_at_entry)
    {
      tss->stdc_raise_records = tss->stdc_raise_records->prev;
    }
  }

  // Returns the newest in-flight stdc_raise() record of the calling thread for
  // `signo`, or NULL when this delivery was not caused by a stdc_raise(). The
  // caller must mark the record consumed and copy its raw_info/raw_context out
  // before walking any deciders: a decider may longjmp out (recovery), and the
  // record lives on the stdc_raise() caller's stack. Records are per-thread, so
  // a delivery only ever sees this thread's raises; "newest" is what makes a
  // rethrow from a decider (which pushes its own record, carrying the original
  // or supplied information) resolve to the rethrow rather than the raise it
  // rethrows.
  static struct WG14_SIGNALS_PREFIX(stdc_raise_record_t) *
  WG14_SIGNALS_PREFIX(find_stdc_raise_record)(const int signo,
                                              const void *token)
  {
#if !defined(__GLIBC__)
    (void) token;
#endif
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state_if_exists)();
    if(tss != WG14_SIGNALS_NULLPTR)
    {
      struct WG14_SIGNALS_PREFIX(stdc_raise_record_t) *record =
      tss->stdc_raise_records;
      while(record != WG14_SIGNALS_NULLPTR)
      {
        if(record->consumed || record->signo != signo)
        {
          record = record->prev;
          continue;
        }
#if defined(__GLIBC__)
        if(record->token == (uintptr_t) token)
        {
          return record;
        }
        record = record->prev;
#else
      return record;
#endif
      }
    }
    return WG14_SIGNALS_NULLPTR;
  }

  // The base signal handler for POSIX
  // You must NOT do anything async signal unsafe in here!
  static void WG14_SIGNALS_PREFIX(raw_signal_handler)(int signo,
                                                      siginfo_t *siginfo,
                                                      void *context)
  {
    // A delivery caused by stdc_raise() hands the deciders exactly the
    // caller-supplied raw_info and raw_context in place of the kernel's own
    // siginfo_t/ucontext_t, which describe the raise rather than the caller's
    // information. A NULL argument is surfaced as NULL -- the decider then sees
    // a NULL raw_info member with error_code zero and addr NULL (N3924
    // 7.14.2.10 p4). The record is marked consumed here, before any decider
    // runs, so a re-entrant delivery (SA_NODEFER) or a rethrow cannot reuse it;
    // the stdc_raise() call that pushed it removes it when it returns, or the
    // guard frame's seq snapshot removes it if a decider longjmps out. An
    // ordinary delivery finds no record and keeps the OS values untouched.
    struct WG14_SIGNALS_PREFIX(stdc_raise_record_t) *record =
    WG14_SIGNALS_PREFIX(find_stdc_raise_record)(signo,
#if defined(__GLIBC__)
                                                (const void *) siginfo->si_ptr
#else
                                              (const void *)
                                              siginfo->si_value.sival_ptr
#endif
    );
    if(record != WG14_SIGNALS_NULLPTR)
    {
      record->consumed = true;
      siginfo = record->raw_info;
      if(record->raw_context != WG14_SIGNALS_NULLPTR)
      {
        context = (void *) record->raw_context;
      }
    }
    if(!WG14_SIGNALS_PREFIX(sighandle)(
       signo, siginfo, (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) context))
    {
      // sighandle() returned false: the map has no entry covering this signal
      // (or the entry is transiently absent under a concurrent
      // install/uninstall, or the per-thread state setup failed). This
      // shouldn't happen — raw_signal_handler is only installed for signals the
      // library manages — but when it does we must pass the signal on to its
      // default action rather than swallow it, and must NOT permanently reset
      // the kernel handler to SIG_DFL: the previous code installed SIG_DFL and
      // then took the default action, which silently discarded the library's
      // installed handler even for a default-ignore signal where nothing was
      // re-raised (analysis UNKN). invoke_sigaction()'s SIG_DFL branch performs
      // the reset-and-raise and restores the current handler afterwards.
      struct sigaction dfl;
      WG14_SIGNALS_MEMSET(&dfl, 0, sizeof(dfl));
      dfl.sa_handler = SIG_DFL;
      (void) WG14_SIGNALS_PREFIX(invoke_sigaction)(&dfl, signo, siginfo,
                                                   context);
    }
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
      WG14_SIGNALS_ABORT();
    }
    if(0 != WG14_SIGNALS_PREFIX(sig_global_tss_state_init)())
    {
      return WG14_SIGNALS_PREFIX(sigguarded_failure_value)();
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *old =
    tss->front,
                                                                       current;
    WG14_SIGNALS_MEMSET(&current, 0, sizeof(current));
    current.prev = old;
    current.guarded = signals;
    current.recovery = recovery;
    current.decider = decider;
    current.rsi.value = value;
    // Snapshot the per-thread raise sequence: a non-local jump below releases
    // this thread's records created after this point, whose stdc_raise() call
    // is being unwound (see release_stdc_raise_records_after).
    current.stdc_raise_seq_at_entry = tss->stdc_raise_seq;
    if(WG14_SIGNALS_SETJMP(current.buf) != 0)
    {
      tss->front = old;
      WG14_SIGNALS_PREFIX(release_stdc_raise_records_after)(
      tss, current.stdc_raise_seq_at_entry);
      // Technically needed to ensure previous handler is active before recovery
      // function is called, as it may raise a signal
      atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
      return recovery(&current.rsi);
    }
    tss->front = &current;
    // Technically needed to ensure setjmp buffer written out before guarded
    // function is called
    atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) ret = guarded(value);
    tss->front = old;
    atomic_signal_fence(WG14_SIGNALS_ATOMIC_PREFIX memory_order_acq_rel);
    return ret;
  }

  // Fills `tss->default_context` with a context object representing the
  // calling stack frame: the implementation-defined default raw_context for a
  // NULL argument (N3924 7.14.2.9 p8, 7.14.2.10 p6). getcontext() captures the
  // real machine state on the platforms where it is sound; macOS's is
  // deprecated and broken on arm64 (it writes an mcontext_t past the caller's
  // ucontext_t), so the calling frame is captured there directly from the frame
  // pointer, stack pointer and return address into the per-thread mcontext
  // storage. It is intentionally not marked async-signal-safe: getcontext() is
  // not in POSIX's async-signal-safe set, but sighandle() is only "usually"
  // async signal safe (see the header) and this runs only on the NULL-context
  // path.
  static void WG14_SIGNALS_PREFIX(construct_default_context)(
  struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) * tss)
  {
    WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *uc = &tss->default_context;
    WG14_SIGNALS_MEMSET(uc, 0, sizeof(*uc));
#if defined(__APPLE__)
    WG14_SIGNALS_MEMSET(&tss->default_mcontext, 0,
                        sizeof(tss->default_mcontext));
    uc->uc_mcontext = &tss->default_mcontext;
    uc->uc_mcsize = sizeof(tss->default_mcontext);
    uc->uc_link = WG14_SIGNALS_NULLPTR;
    uc->uc_stack.ss_sp = __builtin_frame_address(0);
    uc->uc_stack.ss_size = 0;
    uc->uc_stack.ss_flags = 0;
#if defined(__aarch64__)
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    uc->uc_mcontext->__ss.__sp = sp;
    uc->uc_mcontext->__ss.__fp = (uintptr_t) __builtin_frame_address(0);
    uc->uc_mcontext->__ss.__lr = (uintptr_t) __builtin_return_address(0);
    uc->uc_mcontext->__ss.__pc = (uintptr_t) __builtin_return_address(0);
#elif defined(__x86_64__)
    uintptr_t sp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
    uc->uc_mcontext->__ss.__rsp = sp;
    uc->uc_mcontext->__ss.__rbp = (uintptr_t) __builtin_frame_address(0);
    uc->uc_mcontext->__ss.__rip = (uintptr_t) __builtin_return_address(0);
#else
#error "Unsupported macOS architecture for the default raw_context"
#endif
#else
  (void) getcontext(uc);
#endif
  }

  // You must NOT do anything async signal unsafe in here!
  bool WG14_SIGNALS_PREFIX(sighandle)(
  int signo, WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * info,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context)
  {
    // This isn't async signal safe, but caller may not have called
    // siginstall() so we have no other choice within this
    // library
    if(0 != WG14_SIGNALS_PREFIX(sig_tss_state_init_or_set_errno)())
    {
      // The per-thread TSS setup failed (e.g. OOM). The return value alone
      // cannot distinguish this from "no decider installed for this signal"
      // (both are false), so report the failure via errno (see the helper).
      return false;
    }
    if(signo == 0)
    {
      // Caller is doing the non-async safe setup
      return false;
    }
    if(!WG14_SIGNALS_PREFIX(is_valid_signo)(signo))
    {
      // No frame can guard an out-of-range signo and no handler can be
      // installed for one. Reject before the frame walk: the
      // signo-to-sighandler map's get() would report absence anyway, so this is
      // the documented "no decider installed for that signal" return. (The
      // shared helper also documents the macOS/BSD SIGISMEMBER shift-count UB,
      // plans/analysis.md NEGS.)
      return false;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state)();
    // N3924 7.14.2.9 p7/p8/p9: a NULL raw_info/raw_context is replaced by a
    // constructed implementation-defined default, so deciders never see NULL.
    // The defaults live in the per-thread state so a pointer to them outlives
    // this call: the frame path copies the rsi into frame->rsi and can longjmp
    // to a recovery routine that reads them later.
    WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) *eff_info = info;
    if(eff_info == WG14_SIGNALS_NULLPTR)
    {
      WG14_SIGNALS_MEMSET(&tss->default_info, 0, sizeof(tss->default_info));
      tss->default_info.si_signo = signo;
      tss->default_info.si_code = SI_USER;
      eff_info = &tss->default_info;
    }
    WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *eff_context = raw_context;
    if(eff_context == WG14_SIGNALS_NULLPTR)
    {
      // Construct a context object representing the calling stack frame, rather
      // than leaving deciders with a NULL or zeroed context (N3924 7.14.2.9
      // p8). It lives in the per-thread state so the pointer also survives a
      // recovery longjmp.
      WG14_SIGNALS_PREFIX(construct_default_context)(tss);
      eff_context = &tss->default_context;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_per_frame_t) *frame =
    tss->front;
    while(frame != WG14_SIGNALS_NULLPTR)
    {
      if(WG14_SIGNALS_SIGISMEMBER(frame->guarded, signo))
      {
        // With SA_NODEFER a second delivery of a guarded signal re-enters here
        // on the same frame while the outer decider is still executing. A
        // shared `frame->rsi` would be overwritten mid-call (analysis NSTR), so
        // give each raise its own `rsi`; the value is the frame's (pre-set by
        // sigguarded). `frame->rsi` is written only when recovery is requested,
        // so the recovery routine (read post-longjmp from the sigguarded frame)
        // sees exactly the raising raise's info.
        struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
        WG14_SIGNALS_PREFIX(prepare_rsi)(&rsi, signo, eff_info, eff_context);
        rsi.value = frame->rsi.value;
        // In case they wish to abandon
        rsi.internal_local_decider = frame;
        switch(frame->decider(&rsi))
        {
        case WG14_SIGNALS_PREFIX(sig_decision_next_decider):
          break;
        case WG14_SIGNALS_PREFIX(sig_decision_resume_execution):
          frame = frame->prev;
          return true;
        case WG14_SIGNALS_PREFIX(sig_decision_call_recovery):
          if(frame->recovery == WG14_SIGNALS_NULLPTR)
          {
            // No recovery routine: fall through to the outer frame / global
            // deciders / previously installed handler instead of returning,
            // which would make the faulting instruction re-execute forever
            // (analysis.md 1.7/C1).
            frame = frame->prev;
            continue;
          }
          // Copy the siginfo into the storage in the frame for recovery to use
          frame->rsi = rsi;
          WG14_SIGNALS_LONGJMP(frame->buf, 1);
        }
      }
      frame = frame->prev;
    }

    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    LOCK(state->lock);
    WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_itr)
    it = WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_get)(
    &state->signo_to_sighandler_map, signo);
    if(WG14_SIGNALS_PREFIX(signo_to_sighandler_map_t_is_end)(it))
    {
      // We don't have a handler installed for that signal
      UNLOCK(state->lock);
      return false;
    }
    struct WG14_SIGNALS_PREFIX(sighandler_info) *item =
    signo_to_sighandler_map_t_value(it);
    struct sigaction sa = item->old_handler;
    struct WG14_SIGNALS_PREFIX(stdc_siginfo) rsi;
    WG14_SIGNALS_PREFIX(prepare_rsi)(&rsi, signo, eff_info, eff_context);
    // Take a reference on the container for the duration of the raise so a
    // concurrent siguninstall cannot free it while we are unlocked inside a
    // decider call (analysis.md 2.2).
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
          return true;
        }
      } while(current != WG14_SIGNALS_NULLPTR);
    }
    // None of our deciders want this, so call previously installed signal
    // handler
    WG14_SIGNALS_PREFIX(sighandler_info_release)(item);
    UNLOCK(state->lock);
    WG14_SIGNALS_PREFIX(invoke_sigaction)(&sa, signo, eff_info, eff_context);
    return true;
  }

  // You must NOT do anything async signal unsafe in here!
  int WG14_SIGNALS_PREFIX(stdc_raise)(
  int signo, WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) * info,
  WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) * raw_context)
  {
    // Behave as if raise(signo) was called, with the caller's raw_info and
    // raw_context made available to the signal deciders (N3924 7.14.2.10).
    // Unlike sighandle(), which invokes the decider chain directly, this
    // delivers the signal for real through the kernel: the raw handler runs
    // synchronously on this thread, and a record pushed onto this thread's
    // existing stdc_raise() chain tells it to hand the deciders the caller's
    // raw_info/raw_context in place of the kernel's own siginfo_t/ucontext_t.
    //
    // Nothing is allocated and no lock is taken: the record lives in this
    // frame and is linked into the per-thread state only when that state
    // already exists (sig_global_tss_state_if_exists(), the async-signal-safe
    // read of the state sighandle(0, NULL, NULL)/sigguarded() set up). A thread
    // that has never set its state up simply raises without a record, exactly
    // as raise() would. The record is removed here whether or not the handler
    // ran; if a decider longjmps out before this returns, the guard frame's
    // sequence snapshot removes it (release_stdc_raise_records_after()), so
    // nothing this call stacked can outlive it or need unstacking by the
    // signal handling. The token is stack-aware rather than thread-aware: it is
    // the address of the record in this frame, delivered with the signal (via
    // WG14_SIGNALS_RAISE_WITH_TOKEN(), pthread_sigqueue on glibc) so the raw
    // handler can pick this raise's record out of the chain and tell one raise
    // of a signal apart from another. Where the host has no token-carrying
    // delivery the record is matched by signal number alone.
    if(!WG14_SIGNALS_PREFIX(is_valid_signo)(signo))
    {
      // Not a valid signal number: unsuccessful, and no signal is raised
      // (7.14.2.10 p5). No frame can guard an out-of-range signo and no handler
      // can be installed for one (analysis.md NEGS).
      errno = EINVAL;
      return -1;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
    WG14_SIGNALS_PREFIX(sig_global_tss_state_if_exists)();
    struct WG14_SIGNALS_PREFIX(stdc_raise_record_t) record;
    union sigval raise_token;
    raise_token.sival_ptr = WG14_SIGNALS_NULLPTR;
    if(tss != WG14_SIGNALS_NULLPTR)
    {
      // N3924 7.14.2.10 p5/p7: a NULL raw_info is replaced by a constructed
      // implementation-defined default. It lives in the per-thread state, not
      // in this frame, so a pointer to it survives a recovery longjmp that
      // unwinds this stdc_raise() call before the recovery routine reads it.
      WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) *use_info = info;
      if(use_info == WG14_SIGNALS_NULLPTR)
      {
        WG14_SIGNALS_MEMSET(&tss->default_info, 0, sizeof(tss->default_info));
        tss->default_info.si_signo = signo;
        tss->default_info.si_code = SI_USER;
        use_info = &tss->default_info;
      }
      record.prev = tss->stdc_raise_records;
      record.signo = signo;
      record.raw_info = use_info;
      record.raw_context = raw_context;
      record.token = (uintptr_t) &record;
      record.seq = ++tss->stdc_raise_seq;
      record.consumed = false;
      tss->stdc_raise_records = &record;
      // The raw handler reads this back from the delivered signal's
      // si_value.sival_ptr (on glibc) to match the record; it is also the
      // stack-aware identity a rethrow must not reuse.
      raise_token.sival_ptr = (void *) record.token;
    }
    // Self-delivery to the calling thread. For an unblocked signal the handler
    // runs synchronously before this returns and consumes the record; if the
    // caller has blocked the signal, or the disposition is no longer this
    // library's raw handler, the delivery never consults it and the record is
    // removed below all the same. The token is carried where the host provides
    // a token-carrying self-delivery (pthread_sigqueue on glibc); the fallback
    // WG14_SIGNALS_KILL_SELF() has no token and the raw handler then matches by
    // signal number alone.
    const int rc = WG14_SIGNALS_RAISE_WITH_TOKEN(signo, raise_token);
    if(tss != WG14_SIGNALS_NULLPTR)
    {
      // Remove our record, plus any records left above it by a non-local jump
      // that skipped their removal. The stack-aware token identifies the
      // record; the per-thread seq guards against two raises sharing a token
      // (e.g. a rethrow that passes the original raw_info/raw_context).
      struct WG14_SIGNALS_PREFIX(stdc_raise_record_t) *head =
      tss->stdc_raise_records;
      while(head != WG14_SIGNALS_NULLPTR)
      {
        const bool ours =
        head->token == record.token && head->seq == record.seq;
        tss->stdc_raise_records = head->prev;
        if(ours)
        {
          break;
        }
        head = tss->stdc_raise_records;
      }
    }
    return (rc == 0) ? 0 : -1;
  }

  // You must NOT do anything async signal unsafe in here!
  void WG14_SIGNALS_PREFIX(sigdecider_abandon)(
  struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
  {
    if(!rsi->internal_decider_is_abandoned)
    {
      if(rsi->internal_local_decider != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
        WG14_SIGNALS_PREFIX(sig_global_tss_state)();
        if(tss->front != rsi->internal_local_decider)
        {
          // sigdecider_abandon not called on topmost sigguarded()
          assert(tss->front == rsi->internal_local_decider);
          WG14_SIGNALS_ABORT();
        }
        // Pop the top most sigguarded()
        tss->front = tss->front->prev;
        // The decider may longjmp away without ever returning through this
        // delivery, unwinding stdc_raise() records created inside the frame;
        // release them (see release_stdc_raise_records_after).
        WG14_SIGNALS_PREFIX(release_stdc_raise_records_after)(
        tss, rsi->internal_local_decider->stdc_raise_seq_at_entry);
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
      if(rsi->internal_local_decider != WG14_SIGNALS_NULLPTR)
      {
        struct WG14_SIGNALS_PREFIX(sig_global_state_tss_state_t) *tss =
        WG14_SIGNALS_PREFIX(sig_global_tss_state)();
        if(tss->front != rsi->internal_local_decider->prev)
        {
          // sigdecider_abandon not called on topmost sigguarded()
          assert(tss->front == rsi->internal_local_decider->prev);
          WG14_SIGNALS_ABORT();
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

  int WG14_SIGNALS_PREFIX(siginstall_set_sa_flags_np)(int sa_flags)
  {
    if(0 == (sa_flags & SA_SIGINFO))
    {
      // The raw handler is installed with sa_sigaction and receives a
      // siginfo_t *, so a flag set without SA_SIGINFO would break the handler
      // signature. Refuse it without touching the stored flags.
      errno = EINVAL;
      return -1;
    }
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    LOCK(state->lock);
    state->raw_handler_sa_flags = sa_flags;
    UNLOCK(state->lock);
    return 0;
  }

  int WG14_SIGNALS_PREFIX(siginstall_set_default_action_np)(
  WG14_SIGNALS_PREFIX(sig_default_action_np_t) fn)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    atomic_store_explicit(&state->default_action, fn,
                          WG14_SIGNALS_ATOMIC_PREFIX memory_order_release);
    return 0;
  }

  static bool WG14_SIGNALS_PREFIX(install_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    struct WG14_SIGNALS_PREFIX(sig_global_state_t) *state =
    WG14_SIGNALS_PREFIX(sig_global_state)();
    int sa_flags = state->raw_handler_sa_flags;
    if(sa_flags == 0)
    {
      // No siginstall_set_sa_flags_np() call: the library default. Zero is
      // never a legitimate stored value (the setter rejects a flag set
      // without SA_SIGINFO), so it doubles as the unset marker.
      sa_flags = SA_SIGINFO | SA_NOCLDWAIT | SA_NODEFER;
    }
    struct sigaction sa;
    WG14_SIGNALS_MEMSET(&sa, 0, sizeof(sa));
    sa.sa_sigaction = WG14_SIGNALS_PREFIX(raw_signal_handler);
    sa.sa_flags = sa_flags;
    if(-1 == WG14_SIGNALS_SIGACTION(signo, &sa, &item->old_handler))
    {
      return false;
    }
    return true;
  }

  static bool WG14_SIGNALS_PREFIX(uninstall_sighandler_impl)(
  struct WG14_SIGNALS_PREFIX(sighandler_info) * item, const int signo)
  {
    (void) WG14_SIGNALS_SIGACTION(signo, &item->old_handler,
                                  WG14_SIGNALS_NULLPTR);
    return true;
  }

#ifdef __cplusplus
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif


#ifdef __cplusplus
}
#endif

#endif
