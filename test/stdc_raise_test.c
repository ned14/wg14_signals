#define _CRT_SECURE_NO_WARNINGS 1

#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

// Tests for stdc_raise() (N3924 7.14.2.10): it behaves as if raise(signo) was
// called, with the caller's raw_info/raw_context made available to the signal
// deciders. A NULL argument is replaced by a constructed implementation-defined
// default (7.14.2.10 p5/p6/p7), so a decider never sees a NULL raw_info or
// raw_context; when raw_info is NULL the default has error_code zero and addr
// NULL.
//
// POSIX: the signal is delivered for real through the kernel and matched to the
// raise through the calling thread's record chain (the record lives in the
// raise's own frame and its stack address is carried in si_value.sival_ptr
// where the host provides a token-carrying self-delivery). A recovery longjmp
// out of the delivery must not leave a record behind, and a raise on a thread
// that has never called this library still succeeds. An out-of-range signo is
// rejected with EINVAL and raises nothing.
//
// Windows: stdc_raise() raises the corresponding Win32 structured exception.
// The invalid-signo contract is checked cross-platform; the exception path is
// exercised by the sigguarded test machinery and the wine CI leg.

// Fil-C's runtime forbids user handlers for SIGILL (sigaction returns ENOSYS),
// so the in-tree tests raise SIGUSR2 there; SIGILL is fine everywhere else
// (SEH-mapped to EXCEPTION_ILLEGAL_INSTRUCTION on Windows).
#ifdef __FILC__
#define SIGNAL_TO_USE SIGUSR2
#else
#define SIGNAL_TO_USE SIGILL
#endif

// TSan's runtime wraps signal handlers and defers a same-signal delivery made
// from inside a handler until the outer handler returns. The two rethrow tests
// assert synchronous nested delivery, so they are compiled out under TSan;
// every other configuration runs them.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STDC_RAISE_TEST_TSAN 1
#endif
#endif
#if !defined(STDC_RAISE_TEST_TSAN) && defined(__SANITIZE_THREAD__)
#define STDC_RAISE_TEST_TSAN 1
#endif

#ifdef _WIN32

struct win_observed_t
{
  int decider_calls;
  int signo;
  void *raw_info;
  void *raw_context;
};

static struct win_observed_t win_observed;
static int win_raise_result = -99;

static enum WG14_SIGNALS_PREFIX(sig_decision)
win_capture_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  win_observed.decider_calls++;
  win_observed.signo = rsi->signo;
  win_observed.raw_info = rsi->raw_info;
  win_observed.raw_context = rsi->raw_context;
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
win_noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
win_guarded_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  // A real structured-exception raise of an installed signal: the frame decider
  // observes the OS EXCEPTION_RECORD and a non-NULL context for the current
  // call stack, and claims the raise so stdc_raise() returns 0. A NULL
  // raw_info/raw_context is replaced by the constructed default (7.14.2.10
  // p5/p6): the synthesised record makes the context carried in the record's
  // parameters unnecessary, and prepare_rsi() falls back to the delivered
  // ContextRecord.
  win_raise_result = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR, WG14_SIGNALS_NULLPTR);
  return value;
}

int main(void)
{
  int ret = 0;

  // Not a valid signal number: unsuccessful, and no signal is raised
  // (7.14.2.10 p8).
  SECTION("stdc_raise() rejects an invalid signal number");
  {
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(-1, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(NSIG, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
  }

  // A real structured-exception raise: the frame decider observes the OS
  // EXCEPTION_RECORD and a non-NULL context, and claims the raise.
  SECTION("stdc_raise() raises a real structured exception");
  {
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers != WG14_SIGNALS_NULLPTR);
    if(handlers != WG14_SIGNALS_NULLPTR)
    {
      memset(&win_observed, 0, sizeof(win_observed));
      union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
      (void) WG14_SIGNALS_PREFIX(sigguarded)(&guarded, win_guarded_raise,
                                             win_noop_recovery,
                                             win_capture_decider, value);
      CHECK(win_observed.decider_calls == 1);
      CHECK(win_observed.signo == SIGNAL_TO_USE);
      CHECK(win_observed.raw_info != WG14_SIGNALS_NULLPTR);
      CHECK(win_observed.raw_context != WG14_SIGNALS_NULLPTR);
      CHECK(win_raise_result == 0);
      CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);
    }
  }

  return ret;
}

#else

#include <pthread.h>

struct observed_t
{
  int decider_calls;
  int signo;
  WG14_SIGNALS_PREFIX(stdc_siginfo_error_code_t) error_code;
  void *addr;
  void *raw_info;
  void *raw_context;
  // Captured by value inside the decider: raw_info may point at an object whose
  // storage the library owns, so dereferencing it after the raise returns is
  // not guaranteed.
  int raw_si_signo;
  int raw_si_code;
};

static struct observed_t observed;
static siginfo_t fake_info;
static int fake_context;
static siginfo_t frame_test_info;

static enum WG14_SIGNALS_PREFIX(sig_decision)
capture_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  observed.decider_calls++;
  observed.signo = rsi->signo;
  observed.error_code = rsi->error_code;
  observed.addr = rsi->addr;
  observed.raw_info = rsi->raw_info;
  observed.raw_context = rsi->raw_context;
  if(rsi->raw_info != WG14_SIGNALS_NULLPTR)
  {
    observed.raw_si_signo = ((siginfo_t *) rsi->raw_info)->si_signo;
    observed.raw_si_code = ((siginfo_t *) rsi->raw_info)->si_code;
  }
  // Claim the signal so the raise completes and stdc_raise() returns.
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
noop_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
frame_guarded_func(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, &frame_test_info,
  (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context);
  return value;
}

// The recovery and rethrow helpers below, and the corresponding sections in
// main(), are excluded under TSan. The library's recovery longjmps out of the
// signal handler, which leaves TSan's per-thread signal state unbalanced (its
// handler wrapper is bypassed), and the stdc_raise()-driven recovery then hangs
// intermittently; TSan also defers a same-signal delivery made from inside a
// handler until the outer handler returns, so the rethrow tests would not see
// synchronous nested delivery. Fault-driven recovery is still exercised under
// TSan by recovery_null_loop_test; these stdc_raise()-driven paths are covered
// by the default and ASan/UBSan builds.
#ifndef STDC_RAISE_TEST_TSAN
// Requests recovery, which longjmps out of the raise's delivery: nothing the
// raise left behind (its per-thread record) may survive on the unwound stack,
// and the constructed default must still be alive when the recovery reads it.
static int recovery_calls;
static int recovery_observed_info_was_fake;
static int recovery_observed_info_non_null;
static int recovery_observed_error_code_zero;
static int recovery_observed_addr_null;

static enum WG14_SIGNALS_PREFIX(sig_decision)
recover_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  observed.decider_calls++;
  observed.raw_info = rsi->raw_info;
  return WG14_SIGNALS_PREFIX(sig_decision_call_recovery);
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
recover_recovery(const struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  recovery_calls++;
  recovery_observed_info_was_fake = (rsi->raw_info == (void *) &fake_info);
  recovery_observed_info_non_null = (rsi->raw_info != WG14_SIGNALS_NULLPTR);
  recovery_observed_error_code_zero = (rsi->error_code == 0);
  recovery_observed_addr_null = (rsi->addr == WG14_SIGNALS_NULLPTR);
  return rsi->value;
}

static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_recover_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, &fake_info,
  (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context);
  // Unreachable: the decider longjmps to recover_recovery.
  return value;
}

// As above, but with NULL raw_info and raw_context, whose constructed default
// must survive the recovery longjmp.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
guarded_recover_raise_null_info(union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
                                value)
{
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

// After the recovery longjmp, a further raise must still substitute correctly:
// no state from the unwound raise may be left for the handler to match.
static union WG14_SIGNALS_PREFIX(stdc_siginfo_value)
post_recovery_raise(union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value)
{
  (void) WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, &fake_info,
                                         WG14_SIGNALS_NULLPTR);
  return value;
}

// Rethrowing from a decider: a decider that cannot handle the signal re-raises
// it with exactly the information this delivery carried. The rethrow must reach
// the deciders again with the same information, and neither raise's record may
// be left behind.
static int rethrow_decider_calls;
static int rethrow_observed_info_matches;
static int rethrow_observed_context_matches;
static int rethrow_raise_failed;

static enum WG14_SIGNALS_PREFIX(sig_decision)
rethrow_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  rethrow_decider_calls++;
  if(rsi->raw_info == (void *) &fake_info)
  {
    rethrow_observed_info_matches++;
  }
  if(rsi->raw_context == (void *) &fake_context)
  {
    rethrow_observed_context_matches++;
  }
  if(rethrow_decider_calls == 1)
  {
    // Can't handle it after all: rethrow with the information this delivery
    // carried, then claim this delivery too.
    const int rc = WG14_SIGNALS_PREFIX(stdc_raise)(
    SIGNAL_TO_USE,
    (WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) *) rsi->raw_info,
    (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) rsi->raw_context);
    if(rc != 0)
    {
      rethrow_raise_failed = 1;
    }
  }
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

// Rethrowing from a signal handler: an application handler installed before
// siginstall() cannot handle the signal, so the library's deciders are
// consulted first, decline, and the signal is handed off to the application
// handler, which re-raises it with the information it was called with. This is
// the "I can't handle this, rethrow it" idiom stdc_raise() exists to support.
static int handler_rethrow_calls;
static int handler_rethrow_decisions;
static int handler_rethrow_observed_info_matches;
static int handler_rethrow_observed_context_matches;
static int handler_rethrow_raise_failed;

static void app_rethrow_handler(int signo, siginfo_t *si, void *context)
{
  (void) signo;
  handler_rethrow_calls++;
  const int rc = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, (WG14_SIGNALS_PREFIX(stdc_siginfo_siginfo_t) *) si,
  (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) context);
  if(rc != 0)
  {
    handler_rethrow_raise_failed = 1;
  }
}

static enum WG14_SIGNALS_PREFIX(sig_decision)
handler_rethrow_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  handler_rethrow_decisions++;
  if(rsi->raw_info == (void *) &fake_info)
  {
    handler_rethrow_observed_info_matches++;
  }
  if(rsi->raw_context == (void *) &fake_context)
  {
    handler_rethrow_observed_context_matches++;
  }
  if(handler_rethrow_decisions == 1)
  {
    // Decline the original raise so the library hands off to the previously
    // installed application handler, which rethrows it.
    return WG14_SIGNALS_PREFIX(sig_decision_next_decider);
  }
  // Claim the rethrown raise.
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}
#endif

// stdc_raise() must work on a thread whose per-thread state has never been set
// up. It raises for real as raise() would; the caller's information cannot be
// recorded without per-thread state, so that delivery's deciders see the
// kernel's own information, and the handler sets the state up for the next
// raise, which then substitutes normally.
static int fresh_thread_first_result = -99;
static int fresh_thread_second_result = -99;
static int fresh_thread_first_observer_calls = -1;
static int fresh_thread_second_info_matches = -1;

static void *fresh_thread_raise(void *arg)
{
  (void) arg;
  // The first call into this library on this thread is stdc_raise() itself.
  fresh_thread_first_result = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, &fake_info,
  (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context);
  fresh_thread_first_observer_calls = observed.decider_calls;
  // The handler set the thread's state up during the first delivery.
  fresh_thread_second_result = WG14_SIGNALS_PREFIX(stdc_raise)(
  SIGNAL_TO_USE, &fake_info,
  (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context);
  fresh_thread_second_info_matches =
  (observed.raw_info == (void *) &fake_info ? 1 : 0);
  return WG14_SIGNALS_NULLPTR;
}

// Two threads raising the same signal at once: each raise must surface its own
// thread's raw_info/raw_context, never the other thread's, which exercises the
// per-thread matching of the records.
struct concurrent_expectation_t
{
  pthread_t tid;
  siginfo_t *info;
  int context;
  int raises_seen;
  int foreign_info_seen;
};

#define CONCURRENT_RAISES 200

static struct concurrent_expectation_t concurrent_expectations[2];
static siginfo_t concurrent_infos[2];
static pthread_mutex_t concurrent_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t concurrent_cond = PTHREAD_COND_INITIALIZER;
static int concurrent_ready;
static int concurrent_go;

static enum WG14_SIGNALS_PREFIX(sig_decision)
concurrent_decider(struct WG14_SIGNALS_PREFIX(stdc_siginfo) * rsi)
{
  const pthread_t self = pthread_self();
  for(size_t n = 0; n < 2; n++)
  {
    struct concurrent_expectation_t *expectation = &concurrent_expectations[n];
    if(pthread_equal(self, expectation->tid))
    {
      if(rsi->raw_info == (void *) expectation->info &&
         rsi->raw_context == (void *) &expectation->context)
      {
        expectation->raises_seen++;
      }
      else
      {
        expectation->foreign_info_seen++;
      }
      break;
    }
  }
  return WG14_SIGNALS_PREFIX(sig_decision_resume_execution);
}

static void *concurrent_thread(void *arg)
{
  const size_t idx = (size_t) arg;
  concurrent_expectations[idx].tid = pthread_self();
  concurrent_expectations[idx].info = &concurrent_infos[idx];
  memset(&concurrent_infos[idx], 0, sizeof(concurrent_infos[idx]));
  concurrent_infos[idx].si_signo = SIGNAL_TO_USE;
  // The caller's information is recorded in the calling thread's per-thread
  // state, so set that up (the documented `signo == 0` setup form) before the
  // measured raises rather than have the first delivery's handler do it.
  (void) WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                        WG14_SIGNALS_NULLPTR);
  // Start together so the raises overlap as much as possible.
  pthread_mutex_lock(&concurrent_mutex);
  concurrent_ready++;
  pthread_cond_broadcast(&concurrent_cond);
  while(!concurrent_go)
  {
    pthread_cond_wait(&concurrent_cond, &concurrent_mutex);
  }
  pthread_mutex_unlock(&concurrent_mutex);
  for(int n = 0; n < CONCURRENT_RAISES; n++)
  {
    (void) WG14_SIGNALS_PREFIX(stdc_raise)(
    SIGNAL_TO_USE, &concurrent_infos[idx],
    (
    WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &concurrent_expectations[idx]
    .context);
  }
  return WG14_SIGNALS_NULLPTR;
}

int main(void)
{
  int ret = 0;

  // Not a valid signal number: unsuccessful, and no signal is raised
  // (7.14.2.10 p8).
  SECTION("stdc_raise() rejects an invalid signal number");
  {
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(0, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(-1, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(NSIG, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) != 0);
    CHECK(errno == EINVAL);
  }

  // A signal this library has not installed is raised for real as raise()
  // would: no decider runs, and SIGCONT's default action for a running process
  // is to do nothing. The record (none exists: the thread has no state yet) is
  // never consulted.
  SECTION("stdc_raise() of an uninstalled signal");
  {
    memset(&fake_info, 0, sizeof(fake_info));
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
          SIGCONT, &fake_info,
          (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context) == 0);
  }

  void *handlers = WG14_SIGNALS_PREFIX(siginstall)(WG14_SIGNALS_NULLPTR);
  CHECK(handlers != WG14_SIGNALS_NULLPTR);
  if(handlers == WG14_SIGNALS_NULLPTR)
  {
    return ret;
  }
  // The documented `signo == 0` setup form: stdc_raise() records the caller's
  // information in the calling thread's per-thread state, so set it up here.
  CHECK(WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                       WG14_SIGNALS_NULLPTR) == 0);

  // A global decider observing the raise sees exactly the caller-supplied
  // raw_info and raw_context, not the kernel's own (7.14.2.10 p4).
  SECTION("global decider sees the caller's raw_info/raw_context");
  {
    memset(&observed, 0, sizeof(observed));
    memset(&fake_info, 0, sizeof(fake_info));
    fake_info.si_signo = SIGNAL_TO_USE;
    fake_info.si_code = SI_QUEUE;
    fake_info.si_addr = (void *) (intptr_t) 0x5678;
    fake_context = 42;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
          SIGNAL_TO_USE, &fake_info,
          (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context) == 0);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.signo == SIGNAL_TO_USE);
    CHECK(observed.raw_info == (void *) &fake_info);
    CHECK(observed.raw_context == (void *) &fake_context);
    CHECK(observed.error_code == fake_info.si_errno);
    CHECK(observed.addr == fake_info.si_addr);

    WG14_SIGNALS_PREFIX(signal_decider_destroy(decider));
  }

  // NULL raw_info and raw_context are replaced by a constructed default, so the
  // decider sees a non-NULL raw_info (with si_signo set and si_code SI_USER)
  // and a non-NULL raw_context, error_code zero and addr NULL (7.14.2.10
  // p5/p6/p7).
  SECTION("NULL raw_info/raw_context is replaced by a constructed default");
  {
    memset(&observed, 0, sizeof(observed));
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
                                          WG14_SIGNALS_NULLPTR) == 0);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.raw_info != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_context != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_si_signo == SIGNAL_TO_USE);
    CHECK(observed.raw_si_code == SI_USER);
    CHECK(observed.error_code == 0);
    CHECK(observed.addr == WG14_SIGNALS_NULLPTR);

    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
  }

  // A NULL raw_info with a caller raw_context: the context is still surfaced,
  // and the default signal information is constructed as above.
  SECTION("NULL raw_info with a caller raw_context still surfaces it");
  {
    memset(&observed, 0, sizeof(observed));
    fake_context = 42;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
          SIGNAL_TO_USE, WG14_SIGNALS_NULLPTR,
          (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context) == 0);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.raw_info != WG14_SIGNALS_NULLPTR);
    CHECK(observed.raw_context == (void *) &fake_context);
    CHECK(observed.error_code == 0);
    CHECK(observed.addr == WG14_SIGNALS_NULLPTR);

    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
  }

  // A thread whose per-thread state has never been set up: the raise still
  // succeeds as raise() would, but the caller's information cannot be recorded,
  // so that first delivery's decider sees the kernel's own information. The
  // handler sets the state up for the next raise, which substitutes.
  SECTION("a fresh thread with no per-thread state still raises");
  {
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    memset(&observed, 0, sizeof(observed));
    memset(&fake_info, 0, sizeof(fake_info));
    fake_info.si_signo = SIGNAL_TO_USE;
    fake_info.si_code = SI_QUEUE;
    fresh_thread_first_observer_calls = -1;
    fresh_thread_second_info_matches = -1;
    pthread_t thread;
    CHECK(pthread_create(&thread, WG14_SIGNALS_NULLPTR, fresh_thread_raise,
                         WG14_SIGNALS_NULLPTR) == 0);
    CHECK(pthread_join(thread, WG14_SIGNALS_NULLPTR) == 0);
    CHECK(fresh_thread_first_result == 0);
    // The first delivery (no state) still ran the decider once, with the
    // kernel's own information rather than the caller's.
    CHECK(fresh_thread_first_observer_calls == 1);
    CHECK(fresh_thread_second_result == 0);
    CHECK(fresh_thread_second_info_matches == 1);

    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
  }

  // Frame-local deciders observe the caller's information too (the real
  // delivery runs the raw handler, which surfaces it through sighandle).
  SECTION("frame decider sees the caller's raw_info/raw_context");
  {
    memset(&observed, 0, sizeof(observed));
    memset(&frame_test_info, 0, sizeof(frame_test_info));
    frame_test_info.si_signo = SIGNAL_TO_USE;
    frame_test_info.si_code = SI_QUEUE;
    frame_test_info.si_addr = (void *) (intptr_t) 0x1234;
    fake_context = 7;
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    (void) WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, frame_guarded_func, noop_recovery, capture_decider, value);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.signo == SIGNAL_TO_USE);
    CHECK(observed.raw_info == (void *) &frame_test_info);
    CHECK(observed.raw_context == (void *) &fake_context);
    CHECK(observed.addr == (void *) (intptr_t) 0x1234);
  }

#ifndef STDC_RAISE_TEST_TSAN
  // A decider that cannot handle the signal re-raises it with the information
  // the delivery carried; the rethrow reaches the decider again with the same
  // information and neither record is left behind.
  SECTION("rethrow from a decider");
  {
    memset(&observed, 0, sizeof(observed));
    memset(&fake_info, 0, sizeof(fake_info));
    fake_info.si_signo = SIGNAL_TO_USE;
    fake_info.si_code = SI_QUEUE;
    fake_context = 42;
    rethrow_decider_calls = 0;
    rethrow_observed_info_matches = 0;
    rethrow_observed_context_matches = 0;
    rethrow_raise_failed = 0;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, rethrow_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
          SIGNAL_TO_USE, &fake_info,
          (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context) == 0);
    CHECK(rethrow_decider_calls == 2);
    CHECK(rethrow_observed_info_matches == 2);
    CHECK(rethrow_observed_context_matches == 2);
    CHECK(rethrow_raise_failed == 0);

    // No record from either raise is left behind: a follow-up raise with
    // different information must surface that information.
    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
    memset(&observed, 0, sizeof(observed));
    void *decider2 = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, capture_decider, value);
    CHECK(decider2 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(SIGNAL_TO_USE, &fake_info,
                                          WG14_SIGNALS_NULLPTR) == 0);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.raw_info == (void *) &fake_info);
    // The NULL raw_context was replaced by a constructed default context
    // (7.14.2.10 p6), so it is non-NULL.
    CHECK(observed.raw_context != WG14_SIGNALS_NULLPTR);
    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider2);
  }
#endif

  // Two threads at once: each decider call must observe its own thread's
  // pointers, proving a delivery only sees its own thread's records.
  SECTION("concurrent raises are isolated per thread");
  {
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *concurrent = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, concurrent_decider, value);
    CHECK(concurrent != WG14_SIGNALS_NULLPTR);
    memset(concurrent_expectations, 0, sizeof(concurrent_expectations));
    concurrent_ready = 0;
    concurrent_go = 0;
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], WG14_SIGNALS_NULLPTR, concurrent_thread,
                         (void *) (size_t) 0) == 0);
    CHECK(pthread_create(&threads[1], WG14_SIGNALS_NULLPTR, concurrent_thread,
                         (void *) (size_t) 1) == 0);
    pthread_mutex_lock(&concurrent_mutex);
    while(concurrent_ready < 2)
    {
      pthread_cond_wait(&concurrent_cond, &concurrent_mutex);
    }
    concurrent_go = 1;
    pthread_cond_broadcast(&concurrent_cond);
    pthread_mutex_unlock(&concurrent_mutex);
    for(size_t n = 0; n < 2; n++)
    {
      CHECK(pthread_join(threads[n], WG14_SIGNALS_NULLPTR) == 0);
      CHECK(concurrent_expectations[n].raises_seen == CONCURRENT_RAISES);
      CHECK(concurrent_expectations[n].foreign_info_seen == 0);
    }
    WG14_SIGNALS_PREFIX(signal_decider_destroy)(concurrent);
  }

  CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers) == 0);

#ifndef STDC_RAISE_TEST_TSAN
  // A decider that requests recovery unwinds the raise's delivery: the record
  // is released by the handler before the decider walk (or purged by the
  // recovery path), so a later raise still substitutes its own information.
  // Repeat many times: an unreleased record would accumulate in the per-thread
  // chain, so a follow-up raise would not substitute its own information.
  SECTION("recovery longjmp releases the raise record");
  {
    memset(&observed, 0, sizeof(observed));
    memset(&fake_info, 0, sizeof(fake_info));
    fake_info.si_signo = SIGNAL_TO_USE;
    fake_info.si_code = SI_QUEUE;
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers2 = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers2 != WG14_SIGNALS_NULLPTR);
    // A prior siguninstall() destroyed this thread's per-thread state.
    CHECK(WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR) == 0);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    // Repeat many times: an unreleased record would accumulate in the
    // per-thread chain, so a follow-up raise would not substitute its own
    // information.
    for(int n = 0; n < 24; n++)
    {
      recovery_calls = 0;
      (void) WG14_SIGNALS_PREFIX(sigguarded)(&guarded, guarded_recover_raise,
                                             recover_recovery, recover_decider,
                                             value);
      CHECK(recovery_calls == 1);
    }
    CHECK(recovery_observed_info_was_fake != 0);
    // The frame decider that requested recovery observed the caller's info.
    CHECK(observed.decider_calls == 24);
    CHECK(observed.raw_info == (void *) &fake_info);

    // No record may now match: a follow-up raise substitutes its own info.
    observed.decider_calls = 0;
    (void) WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, post_recovery_raise, noop_recovery, capture_decider, value);
    CHECK(observed.decider_calls == 1);
    CHECK(observed.raw_info == (void *) &fake_info);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers2) == 0);
  }

  // A recovery raise with NULL raw_info and raw_context: the constructed
  // default must survive the recovery longjmp, so recovery observes a non-NULL
  // raw_info with error_code zero and addr NULL (7.14.2.10 p5/p7).
  SECTION("recovery with a NULL raw_info sees the constructed default");
  {
    recovery_calls = 0;
    recovery_observed_info_non_null = -1;
    recovery_observed_error_code_zero = 0;
    recovery_observed_addr_null = 0;
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers3 = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers3 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR) == 0);
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    (void) WG14_SIGNALS_PREFIX(sigguarded)(
    &guarded, guarded_recover_raise_null_info, recover_recovery,
    recover_decider, value);
    CHECK(recovery_calls == 1);
    CHECK(recovery_observed_info_non_null == 1);
    CHECK(recovery_observed_error_code_zero != 0);
    CHECK(recovery_observed_addr_null != 0);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers3) == 0);
  }
#endif  // STDC_RAISE_TEST_TSAN (recovery sections)

#ifndef STDC_RAISE_TEST_TSAN
  // The "can't handle it, rethrow it" idiom: an application handler installed
  // before siginstall() declines via the library's deciders first, then
  // re-raises the signal with the information it was handed. The rethrow must
  // reach the deciders again with the same information and unwind cleanly.
  SECTION("rethrow from an application signal handler");
  {
    struct sigaction app_sa;
    memset(&app_sa, 0, sizeof(app_sa));
    app_sa.sa_sigaction = app_rethrow_handler;
    app_sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    CHECK(sigaction(SIGNAL_TO_USE, &app_sa, WG14_SIGNALS_NULLPTR) == 0);
    sigset_t guarded;
    sigemptyset(&guarded);
    sigaddset(&guarded, SIGNAL_TO_USE);
    void *handlers4 = WG14_SIGNALS_PREFIX(siginstall)(&guarded);
    CHECK(handlers4 != WG14_SIGNALS_NULLPTR);
    CHECK(WG14_SIGNALS_PREFIX(sighandle)(0, WG14_SIGNALS_NULLPTR,
                                         WG14_SIGNALS_NULLPTR) == 0);
    memset(&observed, 0, sizeof(observed));
    memset(&fake_info, 0, sizeof(fake_info));
    fake_info.si_signo = SIGNAL_TO_USE;
    fake_info.si_code = SI_QUEUE;
    fake_info.si_addr = (void *) (intptr_t) 0x9abc;
    fake_context = 42;
    handler_rethrow_calls = 0;
    handler_rethrow_decisions = 0;
    handler_rethrow_observed_info_matches = 0;
    handler_rethrow_observed_context_matches = 0;
    handler_rethrow_raise_failed = 0;
    union WG14_SIGNALS_PREFIX(stdc_siginfo_value) value = {.int_value = 7};
    void *decider = WG14_SIGNALS_PREFIX(signal_decider_create)(
    &guarded, false, handler_rethrow_decider, value);
    CHECK(decider != WG14_SIGNALS_NULLPTR);

    CHECK(WG14_SIGNALS_PREFIX(stdc_raise)(
          SIGNAL_TO_USE, &fake_info,
          (WG14_SIGNALS_PREFIX(stdc_siginfo_context_t) *) &fake_context) == 0);
    CHECK(handler_rethrow_calls == 1);
    // Declined once, then the rethrow was claimed.
    CHECK(handler_rethrow_decisions == 2);
    CHECK(handler_rethrow_observed_info_matches == 2);
    CHECK(handler_rethrow_observed_context_matches == 2);
    CHECK(handler_rethrow_raise_failed == 0);

    WG14_SIGNALS_PREFIX(signal_decider_destroy)(decider);
    CHECK(WG14_SIGNALS_PREFIX(siguninstall)(handlers4) == 0);
    CHECK(sigaction(SIGNAL_TO_USE, &app_sa, WG14_SIGNALS_NULLPTR) == 0);
  }
#endif

  return ret;
}

#endif
