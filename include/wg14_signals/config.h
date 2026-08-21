/* Proposed WG14 improved signals support
(C) 2024 - 2026 Niall Douglas <http://www.nedproductions.biz/>
File Created: Nov 2024


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

// #define WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL 0

#ifndef WG14_SIGNALS_CONFIG_H
#define WG14_SIGNALS_CONFIG_H

#ifndef WG14_SIGNALS_PREFIX
#define WG14_SIGNALS_PREFIX(x) x
#endif

#ifndef WG14_SIGNALS_INLINE
#define WG14_SIGNALS_INLINE inline
#endif

//! \brief Compile-time assertion, spelled _Static_assert in C11 and
//! static_assert in C++11 (plans/ideas.md 4.2).
#ifndef WG14_SIGNALS_STATIC_ASSERT
#ifdef __cplusplus
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define WG14_SIGNALS_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif
#endif

#ifndef WG14_SIGNALS_THREAD_LOCAL
#ifdef __cplusplus
#define WG14_SIGNALS_THREAD_LOCAL thread_local
#else
#define WG14_SIGNALS_THREAD_LOCAL _Thread_local
#endif
#endif

#ifndef WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
/* https://maskray.me/blog/2021-02-14-all-about-thread-local-storage
will tell you all you need to know about TLS implementations and
which are async signal safe, and which are not.
*/
#if (defined(__GNUC__) || defined(_MSC_VER)) && !defined(__APPLE__)
#define WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL 1
#else
#define WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL 0
#endif
#endif

#ifndef WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL
#if WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL
#ifdef __GNUC__
// ELF needs to use the initial or local exec TLS model to be async signal safe
//
// WARNING: This can cause issues with this library being loaded dynamically as
// part of a runtime loaded shared library!
#define WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL                                   \
  WG14_SIGNALS_THREAD_LOCAL __attribute__((tls_model("initial-exec")))
#elif defined(_MSC_VER)
// MSVC's thread locals are always async signal safe
#define WG14_SIGNALS_ASYNC_SAFE_THREAD_LOCAL WG14_SIGNALS_THREAD_LOCAL
#endif
#endif
#endif

#ifndef WG14_SIGNALS_NULLPTR
#if __STDC_VERSION__ >= 202300L || __cplusplus
#define WG14_SIGNALS_NULLPTR nullptr
#else
#define WG14_SIGNALS_NULLPTR NULL
#endif
#endif

//! \brief `constexpr` where the language provides one (C++11-or-later, and
//! C23 compilers implementing the N3018 `constexpr` objects), else `const`.
//! For file-scope constant objects in public headers: the object is a
//! compile-time constant where supported, and a plain `const` object
//! otherwise. N3018 is implemented in GCC >= 13, upstream Clang >= 19 and
//! Apple Clang >= 17 (its LLVM 19.1 base); other compilers, including older
//! clang and MSVC, reject the keyword in C mode with "unknown type name
//! 'constexpr'" (cppreference.com/c/compiler_support/23).
#ifndef WG14_SIGNALS_C23_CONSTEXPR_OR_CONST
#if (defined(__cplusplus) && __cplusplus >= 201103L) ||                        \
((defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L) &&                 \
 ((defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13) ||              \
  (defined(__clang__) && __clang_major__ >= 19) ||                             \
  (defined(__apple_build_version__) && __clang_major__ >= 17)))
#define WG14_SIGNALS_C23_CONSTEXPR_OR_CONST constexpr
#else
#define WG14_SIGNALS_C23_CONSTEXPR_OR_CONST const
#endif
#endif

#ifndef WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS
#ifdef _MSC_VER
#define WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS __declspec(selectany)
#else
#define WG14_SIGNALS_IGNORE_MULTIPLE_DEFINITIONS __attribute__((weak))
#endif
#endif

#ifndef WG14_SIGNALS_DEFAULT_VISIBILITY
#ifdef _WIN32
#define WG14_SIGNALS_DEFAULT_VISIBILITY
#else
#define WG14_SIGNALS_DEFAULT_VISIBILITY __attribute__((visibility("default")))
#endif
#endif

#ifndef WG14_SIGNALS_ENABLE_HEADER_ONLY
#define WG14_SIGNALS_ENABLE_HEADER_ONLY 0
#endif

#ifndef WG14_SIGNALS_EXTERN_IMPL
#if WG14_SIGNALS_SOURCE
#ifdef _WIN32
#define WG14_SIGNALS_EXTERN_IMPL extern __declspec(dllexport)
#else
#define WG14_SIGNALS_EXTERN_IMPL extern __attribute__((visibility("default")))
#endif
#else
#define WG14_SIGNALS_EXTERN_IMPL extern
#endif
#endif

#ifndef WG14_SIGNALS_EXTERN
#if WG14_SIGNALS_ENABLE_HEADER_ONLY
// Per-TU static inline: an external-linkage inline function may not call the
// internal static helpers used by the shared implementation
// (-Wstatic-in-inline), and per-TU static definitions avoid duplicate symbols
// when the header is included from more than one translation unit (analysis.md
// 1.8, C3, Y8, Y10).
#define WG14_SIGNALS_EXTERN static WG14_SIGNALS_INLINE
#else
#define WG14_SIGNALS_EXTERN WG14_SIGNALS_EXTERN_IMPL
#endif
#endif

#ifndef WG14_SIGNALS_STDERR_PRINTF
#include <stdio.h>
#define WG14_SIGNALS_STDERR_PRINTF(...) fprintf(stderr, __VA_ARGS__)
#endif

//! \brief Embedder override hooks (the "OS abstraction layer").
//!
//! The POSIX backend calls, by name, the very functions an embedding
//! standard C library may itself be implementing on top of this library:
//! `sigaction()`, `abort()` and `pthread_kill(pthread_self(), ...)`. If
//! those calls resolve to the embedding library's public entrypoints, the
//! library recurses into itself (`sigaction()` calling `sigaction()`,
//! `raise()` calling `stdc_raise()`). Each hook below is a function-like
//! macro defaulting to the standard C/POSIX call; an embedding library
//! redefines the hook to its own internal function by defining the macro
//! before including this header (or on the compiler command line). The
//! replacement function must preserve the guarantees of the API whose
//! implementation it routes (see the ASYNC-SIGNAL-SAFE / THREADSAFE
//! documentation of the public API), and the hooks are `#undef`-safe: the
//! standalone build continues to use the defaults, so the same macro
//! expansion the embedder overrides is what the unmodified reference
//! implementation's own test suite exercises.
//!
//! `WG14_SIGNALS_SIGACTION(signum, act, oldact)` routes every internal
//! `sigaction()` call (install the library's raw handler, query/restore a
//! previous disposition, reset to `SIG_DFL`). An embedding libc whose
//! `sigaction` entrypoint is the registry itself must point this at its
//! kernel-facing syscall wrapper.
//!
//! `WG14_SIGNALS_ABORT()` routes every internal `abort()` call (bad-argument
//! and invariant-violation exits). An embedding libc whose `abort()` raises
//! through this library must point this at its internal abort, which must
//! not loop (it resets to `SIG_DFL` before re-raising).
//!
//! `WG14_SIGNALS_KILL_SELF(signo)` routes the self-delivery used to take a
//! signal's default action (`SIG_DFL`: reset the disposition, re-deliver to
//! this thread, restore the disposition). An embedding libc that does not
//! provide `pthread_kill`/`pthread_self` must point this at its own
//! thread-directed delivery (e.g. the `tgkill`/`gettid` syscall pair).
#ifndef WG14_SIGNALS_SIGACTION
#define WG14_SIGNALS_SIGACTION(signum, act, oldact)                            \
  sigaction(signum, act, oldact)
#endif

#ifndef WG14_SIGNALS_ABORT
#define WG14_SIGNALS_ABORT() abort()
#endif

#ifndef WG14_SIGNALS_KILL_SELF
#define WG14_SIGNALS_KILL_SELF(signo) pthread_kill(pthread_self(), (signo))
#endif

//! \brief `WG14_SIGNALS_GETTID()` routes the thread-id query used by
//! `current_thread_id()` on Linux. An embedding standard C library that does
//! not expose `syscall()` under that name (e.g. LLVM-libc, whose generated
//! `<unistd.h>` declares the fixed-arity `__llvm_libc_syscall` instead) must
//! point this at its own kernel-facing thread-id query (e.g. the
//! `SYS_gettid` syscall). The default is the standard `syscall(SYS_gettid)`.
//! The replacement must be async-signal-safe and thread-safe (it runs inside
//! signal handlers).
#ifndef WG14_SIGNALS_GETTID
#define WG14_SIGNALS_GETTID() syscall(SYS_gettid)
#endif

//! \brief Embedder override hooks for the remaining host calls (the
//! "OS abstraction layer" call-site families, part 2).
//!
//! Besides sigaction/abort/pthread_kill(pthread_self(), ...)/gettid, the
//! POSIX backend calls the C library's memory, sigset, setjmp and
//! pthread-key functions by name. An embedding standard C library whose
//! public entrypoints are the very registry being replaced (or whose
//! internal build provides only C++-linkage symbols for them, as
//! LLVM-libc's hermetic test builds do) must route these calls through its
//! own layer too; each hook below defaults to the standard call and is
//! #undef-safe. The replacements must preserve the guarantees of the
//! ASYNC-SIGNAL-SAFE / THREADSAFE APIs whose implementation they route.
#ifndef WG14_SIGNALS_MEMCPY
#define WG14_SIGNALS_MEMCPY(dest, src, n) memcpy((dest), (src), (n))
#endif
#ifndef WG14_SIGNALS_MEMSET
#define WG14_SIGNALS_MEMSET(dest, c, n) memset((dest), (c), (n))
#endif
#ifndef WG14_SIGNALS_MALLOC
#define WG14_SIGNALS_MALLOC(n) malloc(n)
#endif
#ifndef WG14_SIGNALS_CALLOC
#define WG14_SIGNALS_CALLOC(n, s) calloc((n), (s))
#endif
#ifndef WG14_SIGNALS_FREE
#define WG14_SIGNALS_FREE(p) free(p)
#endif
#ifndef WG14_SIGNALS_SIGEMPTYSET
#define WG14_SIGNALS_SIGEMPTYSET(set) sigemptyset(set)
#endif
#ifndef WG14_SIGNALS_SIGFILLSET
#define WG14_SIGNALS_SIGFILLSET(set) sigfillset(set)
#endif
#ifndef WG14_SIGNALS_SIGADDSET
#define WG14_SIGNALS_SIGADDSET(set, signo) sigaddset((set), (signo))
#endif
#ifndef WG14_SIGNALS_SIGDELSET
#define WG14_SIGNALS_SIGDELSET(set, signo) sigdelset((set), (signo))
#endif
#ifndef WG14_SIGNALS_SIGISMEMBER
#define WG14_SIGNALS_SIGISMEMBER(set, signo) sigismember((set), (signo))
#endif
#ifndef WG14_SIGNALS_PTHREAD_KEY_CREATE
#define WG14_SIGNALS_PTHREAD_KEY_CREATE(key, dtor)                             \
  pthread_key_create((key), (dtor))
#endif
#ifndef WG14_SIGNALS_PTHREAD_ONCE
#define WG14_SIGNALS_PTHREAD_ONCE(once, init) pthread_once((once), (init))
#endif
#ifndef WG14_SIGNALS_PTHREAD_SETSPECIFIC
#define WG14_SIGNALS_PTHREAD_SETSPECIFIC(key, value)                           \
  pthread_setspecific((key), (value))
#endif
#ifndef WG14_SIGNALS_PTHREAD_GETSPECIFIC
#define WG14_SIGNALS_PTHREAD_GETSPECIFIC(key) pthread_getspecific(key)
#endif
#ifndef WG14_SIGNALS_SETJMP
#if WG14_SIGNALS_HAVE__SETJMP
#define WG14_SIGNALS_SETJMP(buf) _setjmp(buf)
#else
#define WG14_SIGNALS_SETJMP(buf) setjmp(buf)
#endif
#endif
#ifndef WG14_SIGNALS_LONGJMP
#if WG14_SIGNALS_HAVE__SETJMP
#define WG14_SIGNALS_LONGJMP(buf, val) _longjmp((buf), (val))
#else
#define WG14_SIGNALS_LONGJMP(buf, val) longjmp((buf), (val))
#endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif


#ifdef __cplusplus
}
#endif

#endif
