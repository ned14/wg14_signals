/* Compile-only coverage for the public header language-mode contract.
   Included by public_headers_modes_test.cmake in c89/gnu89/c11/gnu11. */

#include "wg14_signals/config.h"
#include "wg14_signals/current_thread_id.h"
#include "wg14_signals/thrd_signal_handle.h"
#include "wg14_signals/tss_async_signal_safe.h"

/* Exercise the configuration macros that the headers expose, so an unguarded
   nullptr/thread_local/constexpr or static_assert regression is caught here
   even though the declarations alone would not expand them. */
static void *public_headers_nullptr_value = WG14_SIGNALS_NULLPTR;
static WG14_SIGNALS_THREAD_LOCAL int public_headers_thread_local_value;
WG14_SIGNALS_STATIC_ASSERT(sizeof(void *) >= sizeof(int),
                           "public header config sanity");

int main(void)
{
  int guarded_failure = SIGGUARDED_FAILURE_VALUE.int_value;
  (void) public_headers_nullptr_value;
  (void) public_headers_thread_local_value;
  return (guarded_failure == -99) ? 0 : 1;
}
