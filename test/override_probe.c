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
*/

#include "override_probe.h"

#include <stdlib.h>

int wg14_override_sigaction(int signum, const struct sigaction *act,
                            struct sigaction *oldact)
{
  return sigaction(signum, act, oldact);
}

void wg14_override_abort(void)
{
  abort();
}

int wg14_override_kill_self(int signo)
{
  return pthread_kill(pthread_self(), signo);
}
