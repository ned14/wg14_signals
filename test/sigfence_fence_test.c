/* Regression test for sigfence() (plans/analysis.md 2.9, W11): exercises every
   argument-count overload and, on compilers that use the volatile-sink
   fallback (no GNU extended inline asm, e.g. MSVC), verifies the escape
   guarantee directly: after sigfence(a) the address of the local must have
   been published to the per-TU volatile sink. On GNU/clang the "+m" asm path
   is used, whose memory round-trip is verified by the separate
   sigfence_codegen_test (assembly inspection).

   When WG14_SIGNALS_DISABLE_SIGFENCE_MACRO is defined the sigfence macro does
   not exist (analysis.md AA3), so the test is vacuous: the knob is a
   legitimate config option and must not break the test build. */
#include "wg14_signals/thrd_signal_handle.h"

#ifndef WG14_SIGNALS_DISABLE_SIGFENCE_MACRO
int main(void)
{
  int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
  /* sigfence() (no args) expands a variadic macro with an empty argument
     list, which is a GNU extension outside C23; -Wpedantic therefore warns,
     and the tests build with -Werror (plans/ideas.md 2.5). Suppress exactly
     that diagnostic around the zero-arg overload for both clang and gcc. */
#if defined(__clang__)
#pragma clang diagnostic push
/* -Wvariadic-macro-arguments-omitted is the pre-clang-18 name of the
   zero-argument variadic-macro call diagnostic; clang 18 renamed it to
   -Wgnu-zero-variadic-macro-arguments and rejects the old name as an unknown
   warning group unless -Wunknown-warning-option is itself suppressed (the
   tests build with -Werror). Suppress all three. */
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wvariadic-macro-arguments-omitted"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#endif
  sigfence();
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
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
#else
int main(void)
{
  return 0;
}
#endif
