/* Proposed WG14 improved signals support
(C) 2024 Niall Douglas <http://www.nedproductions.biz/>
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

#ifndef WG14_SIGNALS_ASYNC_SIGNAL_SAFE_TLS_H
#define WG14_SIGNALS_ASYNC_SIGNAL_SAFE_TLS_H

#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

  //! \brief The type of an async signal safe thread local
  typedef struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_s) *
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t);

  /*! \brief The attributes for creating an async signal safe thread local

  The callbacks shall be reentrant (3.23) and shall not introduce data races.
  Neither member may be a null pointer. They may allocate or release storage and
  are not required to be async-signal-safe (N3924 7.30.1 p5).
  */
  struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr)
  {
    //! Create an instance. THREADSAFE REENTRANT
    //!
    //! Invoked with a pointer to a `void *` object. The callback shall store in
    //! that object a pointer to the thread-specific storage it creates and
    //! return zero on success or a nonzero value on failure. On success the
    //! stored pointer shall not be a null pointer; on failure the callback is
    //! responsible for releasing any storage it allocated. The implementation
    //! uses the stored pointer as the thread-specific storage pointer for the
    //! calling thread (N3924 7.30.1 p6).
    int (*const create)(void **dest);
    //! Destroy an instance. THREADSAFE REENTRANT
    //!
    //! Invoked with the thread-specific storage pointer created by the
    //! corresponding call of `create`. The callback shall release the storage
    //! and return zero on success or a nonzero value on failure.
    //! `tss_async_signal_safe_destroy()` returns `thrd_error` if any call of
    //! this callback returns a nonzero value, and `thrd_success` otherwise
    //! (N3924 7.30.1 p7).
    int (*const destroy)(void *v);
  };

  /*! \brief Create an async signal safe thread local instance

  \return `thrd_success` if successful; otherwise `thrd_error` is returned and
  the object pointed to by `val` has an indeterminate representation (N3924
  7.30.6.5 p3).
  */
  WG14_SIGNALS_EXTERN int WG14_SIGNALS_PREFIX(tss_async_signal_safe_create)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) * val,
  const struct WG14_SIGNALS_PREFIX(tss_async_signal_safe_attr) * attr);

  //! \brief Destroy an async signal safe thread local instance
  //!
  //! All threads that initialised this instance must have exited (e.g. been
  //! joined) before this is called: a thread's exit-time deinitialisation
  //! handler may still access the instance otherwise.
  //!
  //! Calling this function twice on the same value is undefined behaviour: the
  //! value is left dangling after this returns, and this reference
  //! implementation deliberately does not guard against its reuse (the C11
  //! `tss_delete` / POSIX `pthread_key_delete` contract likewise leaves a
  //! second deletion undefined; plans/analysis.md `TSSD`). Zero the value
  //! after destroying it if it might be destroyed again.
  //!
  //! \return `thrd_success` if every `destroy` callback invoked by this call
  //! returns zero, and `thrd_error` otherwise (N3924 7.30.6.6 p4).
  WG14_SIGNALS_EXTERN int WG14_SIGNALS_PREFIX(tss_async_signal_safe_destroy)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val);

  /*! \brief THREADSAFE Initialise an async signal safe thread local instance
  for a specific thread

  The initialisation and registration of a thread local state for a specific
  thread is not async signal safe, and so must be manually performed by each
  thread before use of an async signal safe thread local instance. Call this
  function somewhere near the beginning of your thread instance.

  It is safe to call this function many times in a thread.

  \return `thrd_success` if the thread-specific storage pointer exists or was
  created; `thrd_error` if the `create` callback returns a nonzero value or
  stores a null pointer, or if the implementation cannot complete the creation
  after `create` has returned zero. In the latter case the `destroy` callback is
  invoked with the stored pointer before returning (N3924 7.30.6.7 p2, p5).
  */
  WG14_SIGNALS_EXTERN int
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_thread_init)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val);

  /*! \brief THREADSAFE ASYNC-SIGNAL-SAFE Get the thread local value for the
   * current thread.
   */
  WG14_SIGNALS_EXTERN void *WG14_SIGNALS_PREFIX(tss_async_signal_safe_get)(
  WG14_SIGNALS_PREFIX(tss_async_signal_safe_t) val);

#ifdef __cplusplus
}
#endif

#if WG14_SIGNALS_ENABLE_HEADER_ONLY
#include "detail/impl/tss_async_signal_safe.c.ipp"
#endif

#endif
