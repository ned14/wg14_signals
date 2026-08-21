/* Embedder override-hook probe (plans/llvm-project-fork.md Phase 1.2).
(C) 2026 Niall Douglas <http://www.nedproductions.biz/>


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

This header is force-included (via -include /FI) ahead of config.h when
WG14_SIGNALS_OVERRIDE_PROBE=ON. It redefines the three embedder override
hooks to distinct wrapper functions, proving that the unmodified reference
implementation works when an embedding standard C library routes
sigaction()/abort()/pthread_kill(pthread_self(), ...) through its own
machinery. The wrappers are plain pass-throughs to the host libc, which is
exactly what an embedder's kernel-facing layer does.
*/

#ifndef WG14_SIGNALS_TEST_OVERRIDE_PROBE_H
#define WG14_SIGNALS_TEST_OVERRIDE_PROBE_H

#include <pthread.h>
#include <signal.h>

int wg14_override_sigaction(int signum, const struct sigaction *act,
                            struct sigaction *oldact);
void wg14_override_abort(void) __attribute__((noreturn));
int wg14_override_kill_self(int signo);

#define WG14_SIGNALS_SIGACTION(signum, act, oldact)                            \
  wg14_override_sigaction(signum, act, oldact)
#define WG14_SIGNALS_ABORT() wg14_override_abort()
#define WG14_SIGNALS_KILL_SELF(signo) wg14_override_kill_self(signo)

#endif
