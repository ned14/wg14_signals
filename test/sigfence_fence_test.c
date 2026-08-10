/* Regression test for sigfence() (plans/analysis.md 2.9, W11): exercises every
   argument-count overload and, on compilers that use the volatile-sink
   fallback (no GNU extended inline asm, e.g. MSVC), verifies the escape
   guarantee directly: after sigfence(a) the address of the local must have
   been published to the per-TU volatile sink. On GNU/clang the "+m" asm path
   is used, whose memory round-trip is verified by the separate
   sigfence_codegen_test (assembly inspection). */
#include "wg14_signals/thrd_signal_handle.h"

int main(void)
{
  int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvariadic-macro-arguments-omitted"
#endif
  sigfence();
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  sigfence(a);
#if !defined(__GNUC__) && !defined(__clang__)
  // Volatile-sink fallback path: the address of a must have escaped into the
  // sink at slot 0 (the barrier only touches slot 8).
  if(WG14_SIGNALS_PREFIX(sigfence_sink)[0] != (void *) &a)
  {
    return 2;
  }
#endif
  sigfence(a, b);
  sigfence(a, b, c);
  sigfence(a, b, c, d);
  sigfence(a, b, c, d, e);
  sigfence(a, b, c, d, e, f);
  sigfence(a, b, c, d, e, f, g);
  sigfence(a, b, c, d, e, f, g, h);
  return (a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 &&
          h == 8) ?
         0 :
         1;
}
