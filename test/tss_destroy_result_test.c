#include "test_common.h"

#include "wg14_signals/tss_async_signal_safe.h"

#define TSS WG14_SIGNALS_PREFIX(tss_async_signal_safe_t)
#define TSS_ATTR WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
#define TSS_CREATE WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)
#define TSS_DESTROY WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)
#define TSS_THREAD_INIT WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)

static int create_unit(void **dest)
{
  static int value;
  *dest = &value;
  return 0;
}

static int failing_destroy_calls;
static int destroy_fails(void *v)
{
  (void) v;
  failing_destroy_calls++;
  return 1;
}

static int succeeding_destroy_calls;
static int destroy_succeeds(void *v)
{
  (void) v;
  succeeding_destroy_calls++;
  return 0;
}

// 7.30.1 p7 / 7.30.6.6 p4: tss_async_signal_safe_destroy returns thrd_error if
// any destroy callback returns nonzero, and thrd_success only when every
// callback returns zero.
int main(void)
{
  int ret = 0;
  TSS failing = WG14_SIGNALS_NULLPTR;
  TSS succeeding = WG14_SIGNALS_NULLPTR;
  struct TSS_ATTR failing_attr = {create_unit, destroy_fails};
  struct TSS_ATTR succeeding_attr = {create_unit, destroy_succeeds};

  SECTION("Destroy with a failing callback ...");
  CHECK(thrd_success == TSS_CREATE(&failing, &failing_attr));
  CHECK(thrd_success == TSS_THREAD_INIT(failing));
  CHECK(thrd_error == TSS_DESTROY(failing));
  CHECK(failing_destroy_calls == 1);

  SECTION("Destroy with a succeeding callback ...");
  CHECK(thrd_success == TSS_CREATE(&succeeding, &succeeding_attr));
  CHECK(thrd_success == TSS_THREAD_INIT(succeeding));
  CHECK(thrd_success == TSS_DESTROY(succeeding));
  CHECK(succeeding_destroy_calls == 1);

  return ret;
}
