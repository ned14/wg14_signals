#include "test_common.h"

#include "wg14_signals/thrd_signal_handle.h"
#include "wg14_signals/tss_async_signal_safe.h"

#include <signal.h>

#define TSS WG14_SIGNALS_PREFIX(tss_async_signal_safe_t)
#define TSS_ATTR WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
#define TSS_CREATE WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)
#define TSS_DESTROY WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)
#define TSS_THREAD_INIT WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)
#define TSS_GET WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)

static TSS tls;
static volatile sig_atomic_t handler_calls;

struct thread_state
{
  int count;
};

static int create_state(void **dest)
{
  struct thread_state *state =
  (struct thread_state *) calloc(1, sizeof(struct thread_state));
  *dest = state;
  return (state != WG14_SIGNALS_NULLPTR) ? 0 : -1;
}

static int destroy_state(void *v)
{
  free(v);
  return 0;
}

static void handler(int signo)
{
  struct thread_state *state;
  (void) signo;
  state = (struct thread_state *) TSS_GET(tls);
  if(state != WG14_SIGNALS_NULLPTR)
  {
    // 7.30.6.8 p5: the TSS pointer is usable in a handler, but the value it
    // points to is preserved only because the modification is sequenced before
    // a sigfence call. handler_calls is volatile sig_atomic_t.
    state->count++;
    sigfence(state->count);
  }
  handler_calls++;
}

int main(void)
{
  int ret = 0;
  struct TSS_ATTR attr = {create_state, destroy_state};
  void (*old_handler)(int);
  struct thread_state *state;

  CHECK(thrd_success == TSS_CREATE(&tls, &attr));
  CHECK(thrd_success == TSS_THREAD_INIT(tls));
  state = (struct thread_state *) TSS_GET(tls);
  CHECK(state != WG14_SIGNALS_NULLPTR);

  old_handler = signal(SIGINT, handler);
  CHECK(old_handler != SIG_ERR);
  CHECK(0 == raise(SIGINT));
  CHECK(handler_calls == 1);
  CHECK(state->count == 1);

  // Some C runtimes reset the handler after delivery (notably the MSVC CRT,
  // which follows the C89 resetting semantics), so reinstall it before raising
  // again. Reinstalling an already-installed handler is harmless elsewhere.
  CHECK(SIG_ERR != signal(SIGINT, handler));
  CHECK(0 == raise(SIGINT));
  CHECK(handler_calls == 2);
  CHECK(state->count == 2);

  signal(SIGINT, old_handler);
  CHECK(thrd_success == TSS_DESTROY(tls));
  return ret;
}
