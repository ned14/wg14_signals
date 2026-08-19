---
title: "Nxxxx: Wording for \"Thread-safe signals handling rev 5\""
author:
    - Douglas, Niall
date:
    - 2026-07-24
---


## Preamble

### contributing {-}

Niall Douglas (rationale, history)

Niall Douglas, Jens Gustedt (wording)

### Related documents

| number                                                              | Title                                  | Authors        | Remarks                   |
|---------------------------------------------------------------------|----------------------------------------|----------------|---------------------------|
| [N2471](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2471.pdf) | Stackable, thread local, signal guards | Douglas, Niall | revision 0, 2020-02-02    |
| [N3540](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3540.pdf) | Modern signals handling                | Douglas, Niall | revision 1, 2025-05-02    |
| [N3765](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3765.pdf) | Thread-safe signals handling           | Douglas, Niall | revision 2, 2025-12-14    |
| [N3872](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3872.pdf) | Thread-safe signals handling           | Douglas, Niall | revision 3, 2026-04-12    |
| [N3783](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3783.pdf) | Working Draft                          |                | base for diff, 2025-01-15 |
| [N3924](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3924.htm) | &lt;this paper&gt;                     | Wording Group  |                           |

### LaTeX document branch

none

### Liaison

WG21, Austin Group

### Relevant polls

| meeting                                       | date       | for | against | abstain |
|-----------------------------------------------|------------|-----|---------|---------|
| N3924 revision 4                              | ?          | ?   | ?       | ?       |
| N3872 revision 3                              | ?          | ?   | ?       | ?       |
| N3765 revision 2 (March 2026 virtual [N3856]) | 2026-03-09 | 15  | 2       | 4       |
| N3540 revision 1 (Final Fall 2025 [N3815])    | 2025-08-29 | 19  | 2       | 4       |
| N2471 revision 0 (Draft April 2020 [N2519])   | 2020-03-30 | 5   | 2       | 6       |


## Proposed wording

### Legend

Deletions in the shown standard text are as shown <del>here</del>,
additions, as shown <ins>here</ins>. These may render differently
according to the style in which the document is shown by your browser,
but should always be well distinguishable. In the provided style there
are two visual distinctions:

- A high contrast color palete
  ([Okabe and Ito](https://jfly.uni-koeln.de/color/)) namely using colors
  black, <span style="color: #E69F00">orange</span>, <span
  style="color: #009E73">teal green</span> and <span
  style="background: #FFFFD0">light yellow</span>
- normal text, <s>strike through</s>, <u>underlining</u> and
  <tt>typewriter font</tt>.

Close to each other proposed changes <span style="color:
  #009E73"><u>resemble</u></span> <span style="color:
  #E69F00"><s>like</s></span> `this`.


### Modifications in clause 5.2.2.4 paragraph 5

> When the processing of the abstract machine is interrupted by receipt of a signal, the values of objects
> that are <del>neither lock-free atomic objects nor of type `volatile sig_atomic_t`</del>

> <ins>
> not one of the following categories:
>
> 1. A lock-free atomic object.
> 2. Of type `volatile sig_atomic_t`.
> 3. Modified before a call to `sigfence()`.
> 4. The object designated by the `void *` returned by `tss_async_signal_safe_get()`.
> </ins>

> are unspecified, as is
> the state of the dynamic floating-point environment. The representation of any object modified by
> the handler that is <del>neither a lock-free atomic object nor of type `volatile sig_atomic_t`</del>

> <ins>not one of the following categories:
>
> 1. A lock-free atomic object.
> 2. Of type `volatile sig_atomic_t`.
> 3. Modified before a call to `sigfence()`.
> </ins>

> becomes
> indeterminate when the handler exits, as does the state of the dynamic floating-point environment if
> it is modified by the handler and not restored to its original state.

### Modifications in clause 7.14.1

> **Paragraph 1.** The header `<signal.h>` declares <del>a type and two functions and
> defines several macros</del> <ins>types, functions and macros</ins>,
> for handling various *signals* (conditions that may be reported
> during program execution).

> **Insert the following new paragraphs after paragraph 1:**

> <ins>
> A signal can be received by a thread within the program.
> When a signal is received, execution is interrupted and the
> currently installed *signal handler* for that signal is called.
>
> There are the following categories of signal:
> </ins>

> <ins>
> 1. *Synchronous:* these are caused by a thread doing something. Standard
> signals are: (i) abnormal termination, (ii) erroneous arithmetic operation,
> (iii) detection of an invalid function image, and (iv) invalid access to storage.
>
>     The implementation may define additional synchronous category signals.
>
> 2. *Asynchronous non-debug:* these are generated by the environment and are not usually generated by testing. Standard signals are: (i) receipt of an interactive attention signal, and (ii) termination request sent to the program.
>
>     The implementation may define additional asynchronous non-debug category signals.
>
> 3. *Asynchronous debug:* these are generated by the environment and
>    are usually generated by testing.
>
>     The implementation may define asynchronous debug category signals.
>
> *Async-signal-safe* functions and macros are those safe to call during the handling of a signal. Only the functions of clause 7 listed below are required to be async-signal-safe; all other functions need not be async-signal-safe. The following functions are required to be async-signal-safe:
>
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument,
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the handler. Furthermore, if such a call to the signal function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation, or
> - any function within this standard explicitly described as async-signal-safe.
>
> There are two ways to change the currently installed signal handler:
>
> 1. The `signal` function globally installs a single signal
> handler for the whole program execution, overwriting any previously set handler.
>
> 2. The `siginstall` function enables an alternative
> signal handling mechanism which implements thread-safe composable signal handling.
> </ins>

> <ins>
> If `siginstall` has not been called in the current program execution, then the following sequence occurs on signal receipt:
>
> 1. If there is such a handler, the most recently installed handler by `signal` for that signal number is called, unless that handler was set to `SIG_IGN`, in which case the signal is ignored.
> The thread in which the handler is called is unspecified.
> 2. Otherwise, if no call of `signal` for the signal number was performed, the handler has `SIG_DFL` semantics, which is the default action for that signal number on that implementation.
>
> If `siginstall` has been called in the current program execution, then the following sequence occurs on signal receipt:
>
> 1. For synchronous category signals, the signal handler shall be called on the thread that caused the signal. For asynchronous category signals, the thread on which the signal handler is called is unspecified.
>
> 2. An ordered sequence of signal deciders is invoked on the thread that received the signal to decide how to handle the signal. The ordered sequence begins with the thread-locally installed signal deciders whose signal set matches the signal number, in order of most recently installed first for that thread, followed by the globally installed signal deciders whose signal set matches the signal number:
>     - For thread-locally installed signal deciders, each decider function is called with a pointer to a valid `stdc_siginfo`, with its `value` member set to the value that was specified when that decider was installed. If a decider function returns:
>         - `sig_decision_resume_execution`: execution of the interrupted thread is resumed.
>         - `sig_decision_call_recovery`: the environment is restored to what it was when that thread-local decider was installed, as if `setjmp` had been called during installation and a `longjmp` to restore that saved environment had been performed, and the recovery function as specified at that time shall be called to implement recovery from the signal raise for that thread.
>         - `sig_decision_next_decider`: the next decider in the sequence is called.<br><br>
>
>     - For globally installed signal deciders, each decider function is called with a pointer to a valid `stdc_siginfo`, with its `value` member set to the value specified when that decider was installed. The deciders are called in the following order: first, those installed with `callfirst == true`, in order of most recently installed first; then, those installed with `callfirst == false`, in order of most recently installed last.
>
>         If any decider function returns:
>
>         - `sig_decision_resume_execution`: execution of the interrupted thread is resumed.
>         - `sig_decision_call_recovery`: an implementation-defined action is performed.
>         - `sig_decision_next_decider`: the next decider in the sequence is called.
>
> It is permitted for a signal decider to never return. Signal deciders shall meet the same requirements as a signal handler, as specified for the `signal` function (7.14.2.3).
> A signal decider that determines that it will never return through the signal decider machinery should call `sigdecider_abandon()` before not returning, and may call `sigdecider_abandon_resume()` to retract that declaration if it later determines that it will return after all (7.14.3.2 and 7.14.3.3).
> If every signal decider returns `sig_decision_next_decider`, the behavior is implementation-defined.
>
> `siginstall` may be called multiple times, and for each a corresponding `siguninstall` should be present in the program. Each call to `siginstall` takes a set of signals for which the thread-safe implementation is to be activated. The thread-safe implementation shall not be deactivated for that signal number until the last uninstallation for that signal number is performed.
> </ins>

> <ins>
> **Recommended practice**

> It is recommended
> that pre-existing programs be upgraded to use `siginstall`,
> and that newly written code prefer `siginstall`
> over `signal`.
> </ins>

> <ins>
> EXAMPLE 1: Use `sigguarded` to recover from a `SIGFPE`:
> </ins>

> <ins>
> ```
> /* Recovery function for SIGFPE */
> static union stdc_siginfo_value
> sigfpe_recovery_func(const struct stdc_siginfo *rsi)
> {
>   /* Recover from the signal raise */
>   return rsi->value;
> }
>
> /* Decider function for SIGFPE */
> static enum sig_decision
> sigfpe_decider_func(struct stdc_siginfo *rsi)
> {
>   /* Verify we got SIGFPE */
>   if(rsi->signo != SIGFPE)
>   {
>     abort();
>   }
>   rsi->value.int_value = SIGFPE;
>   /* Please recover */
>   return sig_decision_call_recovery;
> }
>
> /* Guarded function that triggers SIGFPE via division by zero */
> static union stdc_siginfo_value
> sigfpe_func(union stdc_siginfo_value value)
> {
>   /* volatile is needed to prevent elision under optimization */
>   volatile int divisor = 0;
>   /* This should trigger SIGFPE */
>   volatile int result = 42 / divisor;
>   /* If we get here, this architecture doesn't trap integer divide by zero */
>   stdc_raise(SIGFPE, nullptr, nullptr);
>   return value;
> }
> ...
> int main() {
>   union stdc_siginfo_value value = { .int_value = 0 };
>   sigset_t guarded;
>   sigemptyset(&guarded);
>   sigaddset(&guarded, SIGFPE);
>   value = sigguarded(&guarded,
>                     sigfpe_func,
>                     sigfpe_recovery_func,
>                     sigfpe_decider_func,
>                     value);
>   assert(value.int_value == SIGFPE);
> }
> ```
> </ins>

> **Paragraph 2.** The <del>type defined is</del><ins>types defined are</ins>

> <ins>
> `stdc_siginfo_error_code_t`
> which is an implementation-defined complete native error code type.
> </ins>


> <ins>
> The `stdc_siginfo_value` union shall contain at least a member
> `void *ptr_value`, and, if the implementation has `intptr_t` (7.23.2.5),
> a member `intptr_t int_value`, in any order.
> </ins>

> <ins>
> `stdc_siginfo_siginfo_t`
> which is an implementation-defined possibly incomplete signal information type.
> </ins>

> <ins>
> `stdc_siginfo_context_t`
> which is an implementation-defined possibly incomplete context type.
> </ins>

> <ins>
> The `stdc_siginfo` structure shall contain at least the following members, in any order. The semantics of the members are expressed in the comments.
> </ins>

> <ins>
> ```
> // The signal raised
> int signo;
>
> // The system specific error code for this signal
> stdc_siginfo_error_code_t error_code;
>
> // Memory location which caused fault, if appropriate
> void *addr;
>
> // A user-defined value
> union stdc_siginfo_value value;
>
> // The system specific information. Can be a null pointer if the
> // value was not supplied, or was supplied as null.
> stdc_siginfo_siginfo_t *raw_info;
>
> // The system specific context. Can be a null pointer if the
> // value was not supplied, or was supplied as null.
> stdc_siginfo_context_t *raw_context;
> ```
> </ins>

> <ins>
> The `sig_func_t` type is a function type with a single argument of type
> `union stdc_siginfo_value` and which returns `union stdc_siginfo_value`.
> </ins>

> <ins>
> The `sig_recover_t` type is a function type with a single argument of type
> `const struct stdc_siginfo *` and which returns `union stdc_siginfo_value`.
> </ins>

> <ins>
> The `sig_decision` enumeration shall contain at least the following members, in any order. The semantics of the members are expressed in the comments:
> </ins>

> <ins>
> ```
> // We have decided to do nothing
> sig_decision_next_decider
>
> // We have fixed the cause of the signal, please resume execution
> sig_decision_resume_execution
>
> // thread-local signal deciders only: reset the stack and local
> // state to entry to `sigguarded()`, and call the recovery
> // function.
> sig_decision_call_recovery
> ```
> </ins>

> <ins>
> The `sig_decide_t` type is a function type with a single argument of type
> `struct stdc_siginfo *` and which returns `enum sig_decision`, this being
> the type of the function called by `sigguarded()` and by globally installed
> signal deciders to decide how to handle a raised exception.
> </ins>

> `sig_atomic_t`

> which is the (possibly volatile-qualified) integer type of an object that can be accessed as an atomic entity, even in the presence of asynchronous interrupts.

> <ins>
> `sigset_t`
> </ins>

> <ins>
> which is an implementation-defined complete type able to represent a set of signals on this platform, and for which this code is valid:
> </ins>

> <ins>
> ```
> sigset_t a;
> sigemptyset(&a);
> sigset_t b = a;
> ```
> </ins>

> **After paragraph 2, insert the following new paragraph:**

> <ins>
> `SIGGUARDED_FAILURE_VALUE` is a `constexpr` variable of type `union stdc_siginfo_value` with an implementation-defined value; it is the value returned by `sigguarded()` if it fails to install the guard.
> </ins>

> **Paragraph 3.** The macros defined are

> ```
> SIG_DFL
> SIG_ERR
> SIG_IGN
> ```

> which expand to constant expressions with distinct values that have type compatible with the second argument to, and the return value of, the `signal` function, and whose values compare unequal to the address of any declarable function; and the following,
>  which expand to positive integer constant expressions with type `int` and distinct values that are the signal numbers, each corresponding to the specified condition:

> `SIGABRT` abnormal termination, such as is initiated by the `abort` function<ins>, which is of the *synchronous* signal category</ins>

> `SIGFPE` an erroneous arithmetic operation, such as zero divide or an operation resulting in overflow<ins>, which is of the *synchronous* signal category</ins>

> `SIGILL` detection of an invalid function image, such as an invalid instruction<ins>, which is of the *synchronous* signal category</ins>

> `SIGINT` receipt of an interactive attention signal<ins>, which is of the *asynchronous non-debug* signal category</ins>

> `SIGSEGV` an invalid access to storage<ins>, which is of the *synchronous* signal category</ins>

> `SIGTERM` a termination request sent to the program<ins>, which is of the *asynchronous non-debug* signal category</ins>

> **Insert the following after paragraph 3:**

<ins>
> and the following macro, which restricts the optimizations that the compiler may perform:

> - `sigfence(vars ...)` prevents the compiler from relocating memory accesses from one side of the fence to the other; it also causes the compiler to flush to memory, before the fence, any changes to the following memory, and to reload the following memory from memory after the fence:
>     - the memory storing all objects with external or internal linkage;
>     - the memory storing the objects without linkage named by `vars ...`.
>
>   `sigfence()` is *async-signal-safe*. The macro accepts between zero and eight arguments; any additional arguments cause a diagnostic. Each argument, if any, shall be an lvalue designating an object without linkage.
>   NOTE: `atomic_signal_fence()` provides weaker guarantees than `sigfence()`, and may be sufficient for some performance-oriented use cases.
</ins>

### Insert the signal set functions into clause 7.14.2, renumbering the subsections below accordingly

Insert the following new subsections at the beginning of clause 7.14.2
(Specify signal handling), before "The signal function" (7.14.2.1):

- 7.14.2.1 Signal set initialization
- 7.14.2.2 Signal set manipulation

"The signal function" (7.14.2.1) is renumbered 7.14.2.3, and "The raise
function" (7.14.3.1), together with its section heading "7.14.3 Send
signal", is moved into clause 7.14.2 and renumbered 7.14.2.4. The new
functions specified below are numbered 7.14.2.5 through 7.14.2.9, and the
new section "7.14.3 Recover from signal" is added after clause 7.14.2.

### 7.14.2.1 Signal set initialization

#### The `sigemptyset` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigemptyset(sigset_t *setp);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> The set of signals pointed to by `setp` is set to the empty set as defined by the implementation.

> **Returns**

> This function always returns zero.
</ins>

#### The `sigfillset` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigfillset(sigset_t *setp);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> The set of signals pointed to by `setp` is set to the full set as defined by the implementation.

> **Returns**

> This function always returns zero.
</ins>

#### The `sigfillset_synchronous` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigfillset_synchronous(sigset_t *setp);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> `*setp` is set to exactly the set of synchronous signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> Synchronous signals are those which can be raised by a thread in the course of its execution. This set can include platform-specific additional signals; however, at least these standard signals are within this set: `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGSEGV`.

> **Returns**

> This function always returns zero.
</ins>

#### The `sigfillset_asynchronous_nondebug` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigfillset_asynchronous_nondebug(sigset_t *setp);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> `*setp` is set to exactly the set of non-debug asynchronous signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> Non-debug asynchronous signals are those which are delivered by the system and are not usually generated by testing. This set can include platform-specific additional signals; however, at least these standard signals are within this set: `SIGINT`, `SIGTERM`.

> **Returns**

> This function always returns zero.
</ins>

#### The `sigfillset_asynchronous_debug` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigfillset_asynchronous_debug(sigset_t *setp);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> `*setp` is set to exactly the set of debug asynchronous signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> Debug asynchronous signals are those which are delivered by the system and are usually generated by testing.

> **Returns**

> This function always returns zero.
</ins>

### 7.14.2.2 Signal set manipulation

#### The `sigaddset` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigaddset(sigset_t *setp, int signo);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> Signal number `signo` is added to the set of signals pointed to by `setp`, if it is not already set in which case nothing is done.

> If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**

> If `signo` is a valid signal number, this function returns zero. If `signo` is not a valid signal number, this function returns a negative value.
</ins>

#### The `sigdelset` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigdelset(sigset_t *setp, int signo);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> Signal number `signo` is removed from the set of signals pointed to by `setp`, if it is not already unset in which case nothing is done.

> If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**

> If `signo` is a valid signal number, this function returns zero. If `signo` is not a valid signal number, this function returns a negative value.
</ins>

#### The `sigismember` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int sigismember(const sigset_t *setp, int signo);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*setp`, and is async-signal-safe.

> If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**

> If signal number `signo` is set within the set of signals pointed to by `setp`, positive one is returned. If it is not present, zero is returned. If `signo` is not a valid signal number, a negative value is returned.
</ins>

### Modifications in clause 7.14.2.3: The `signal` function

> ...

> **Description**

> ...

> 5 If the signal occurs other than as the result of calling the `abort` or `raise` function, the behavior is undefined if <del>the signal handler refers to any object with static or thread storage duration that is not a lock-free atomic object and that is not declared with the `constexpr` storage-class specifier other than by assigning a value to an object declared as `volatile sig_atomic_t`, or the signal handler calls any function in the standard library other than</del><ins>the signal handler calls any function in the standard library not described as async-signal-safe other than:</ins>


> - the `abort` function,
> - the `_Exit` function,
> - the `quick_exit` function,
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument, or
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the handler. Furthermore, if such a call to the signal function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation.

> 6 ...

> 7 Use of this function in a multi-threaded program results in undefined behavior. It is also undefined behavior to call this function for a signal number for which the thread-safe implementation has been activated. <del>The implementation shall behave as if no library function calls the signal function.</del>

### Modifications in clause 7.14.2 (continued)


#### In 7.14.2.5: The `siginstall` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> void *siginstall(const sigset_t *guarded);
> ```

> **Description**

> Calling this function is thread-safe.

> If `*guarded` was not previously initialized, the behavior is undefined.

> If `guarded` is a null pointer, the behavior is as if it pointed to a signal set containing all signals.

> For all signals in the signal set `guarded`, the thread-safe implementation shall be activated according to the Introduction above, except that it is implementation-defined which signals are not installed, and for those the signal is silently skipped.

> **Returns**

> If the installation was unsuccessful, this function returns a null pointer.

> If the installation was successful, this function returns a handle to this installation that can be later passed to `siguninstall()`.
</ins>

#### In 7.14.2.6: The `siguninstall` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int siguninstall(void *handle);
> ```

> **Description**

> Calling this function is thread-safe.

> For all signals in the signal set originally installed by the `siginstall()` call that returned `handle`, the thread-safe implementation shall be deactivated according to the Introduction above.

> The handle becomes invalid after a successful call to this function; it is undefined behavior to pass a handle that is not the value returned by a prior `siginstall()` call that has not yet been uninstalled, or to pass a null pointer.

> **Returns**

> If successful, this function returns zero. If unsuccessful, this function returns a negative value.
</ins>


#### In 7.14.2.7: The `signal_decider_create` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> void *signal_decider_create(const sigset_t *guarded, bool callfirst,
>                             sig_decide_t decider,
>                             union stdc_siginfo_value value);
> ```

> **Description**

> Calling this function is thread-safe.

> If `*guarded` was not previously initialized, the behavior is undefined.

> Installs a global signal continuation decider function, which shall be async-signal-safe. See Introduction for how global signal continuation decider functions are called.

> If `callfirst` is true, installs the function at the top of the list to be called before any other functions currently in the list, otherwise it is installed at the end of the list.

> If a signal in `guarded` is not currently installed by `siginstall()` at the time of the call, it is implementation-defined whether a decider installed for it is ever invoked.

> It is undefined behavior to call this function from within a signal decider function.

> **Returns**

> If successful, this function returns a non-null pointer which can be later passed to the `signal_decider_destroy()` function. Otherwise a null pointer is returned.
</ins>

#### In 7.14.2.8: The `signal_decider_destroy` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> int signal_decider_destroy(void *handle);
> ```

> **Description**

> Calling this function is thread-safe.

> Uninstalls a previously installed global signal continuation decider function.

> It is permitted to call this function from within a signal decider function. If the decider whose storage is being destroyed is currently executing, it shall complete its execution and the storage for its entry is released after that decider returns.

> **Returns**

> If successful, this function returns zero. If unsuccessful, this function returns a negative value.
</ins>

#### In 7.14.2.9: The `stdc_raise` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> bool stdc_raise(int signo,
>                 stdc_siginfo_siginfo_t *raw_info,
>                 stdc_siginfo_context_t *raw_context);
> ```

> **Description**

> Calling this function is thread-safe and async-signal-safe. The behavior is undefined if this function is called during the handling of a signal by a thread on which neither this function nor the `sigguarded` function has previously been called.

> This function behaves as if it called the `raise` function (7.14.2.4) with the argument `signo`, but with the added information `raw_info` and `raw_context`.

> Signal deciders are invoked for `signo` only if at least one call to `siginstall` with a signal set containing `signo` has been performed in the current program execution. Otherwise, this function behaves as if it called the `raise` function without invoking any signal deciders.

> The `raw_info` and `raw_context` members of the `stdc_siginfo` passed to deciders are set from the arguments. If `raw_info` is a null pointer, the `raw_info` member is a null pointer, the `error_code` member is zero, and the `addr` member is a null pointer.

> If `signo` is not a valid signal number, this function returns false without raising a signal.

</ins>

Note to implementers:
For your information the reference implementation library does not raise a real
signal on POSIX, but simulates raising one instead because there is no other
way to pass in a custom `siginfo_t` and `ucontext_t`. If it reaches the end of
all lists, it calls the signal handler which was installed before threadsafe signals was
installed. On Microsoft Windows, it does actually raise a real Win32 exception as
for those you can specify a custom `EXCEPTION_RECORD` and `CONTEXT`. As both thread
local and globally installed signal handlers are directly installed with Windows, it
will perform its default action when it runs out of handlers.
Suggestion to implementers: I think it would be preferable if a real signal was
initially raised where possible, then debuggers get notified. You may be able to
persuade your standard C library to implement this e.g. on Linux one can use
syscall `rt_tgsigqueueinfo`.
> <ins>
> **Returns**

> It is implementation-defined if this function ever returns, but if it does, this function returns true if at least one signal decider installed under this facility was called.
> </ins>

### Add 7.14.3 Recover from signal

#### Add 7.14.3.1: The `sigguarded` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> union stdc_siginfo_value
> sigguarded(const sigset_t *signals,
>            sig_func_t guarded,
>            sig_recover_t recovery,
>            sig_decide_t decider,
>            union stdc_siginfo_value value);
> ```

> **Description**

> Calling this function is thread-safe and async-signal-safe. The behavior is undefined if this function is called during the handling of a signal for which no call to `siginstall` with a signal set containing that signal number has been performed in the current program execution. The `decider` function shall be async-signal-safe.

> Installs a thread-local signal continuation decider function, recording the current stack and local state such that they can be restored later. If a decider installed by this call returns `sig_decision_call_recovery`, the environment is restored to what it was when this function was called, as if `setjmp` had been called during installation and a `longjmp` to restore that saved environment had been performed, and the `recovery` function is called to implement recovery from the signal raise. See 7.14.1 for how thread-local signal continuation decider functions are called.

> The behavior is undefined if `signals`, `guarded`, or `decider` is a null pointer.

> The behavior is undefined if `guarded` or `recovery` does not return, or transfers control out of the function other than by returning, unless the `decider` abandons the guard (7.14.3.2).

> If `recovery` is a null pointer, a decider returning `sig_decision_call_recovery` is treated as if it had returned `sig_decision_next_decider`.

> **Returns**

> If no signal was raised, this function returns the value returned by `guarded`. If a signal was raised and a signal decider initiated recovery, this function returns the value returned by `recovery`. If this function fails to install the guard, it returns `SIGGUARDED_FAILURE_VALUE`.
</ins>

#### Add 7.14.3.2: The `sigdecider_abandon` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> void sigdecider_abandon(struct stdc_siginfo *rsi);
> ```

> **Description**

> Calling this function is thread-safe and async-signal-safe.

> The `sigdecider_abandon` function declares that the calling code will never return through the signal decider machinery (7.14.1): the `decider` function of the call to `sigguarded` (7.14.3.1), or a globally installed decider function. The argument `rsi` shall be the `stdc_siginfo` pointer passed to the calling decider function; otherwise the behavior is undefined.

> It is undefined behavior to call this function from within a `recovery` function, or from code that is not currently executing within a decider function.

> If called within a thread-local decider, it shall be the decider of the topmost `sigguarded()` for the calling thread, and the current `sigguarded()` shall be abandoned.

> **Returns**

> This function returns no value.
</ins>

#### Add 7.14.3.3: The `sigdecider_abandon_resume` function

<ins>
> **Synopsis**

> ```
> #include <signal.h>
> void sigdecider_abandon_resume(struct stdc_siginfo *rsi);
> ```

> **Description**

> Calling this function is thread-safe and async-signal-safe.

> The `sigdecider_abandon_resume` function undoes a prior call of `sigdecider_abandon()`: the argument `rsi` shall be the same `stdc_siginfo` pointer passed to a prior call of `sigdecider_abandon()` made in the same decider function call on the calling thread; otherwise the behavior is undefined.

> It is undefined behavior to call this function from a decider function other than the one in which the prior `sigdecider_abandon()` call was made, or to resume an abandonment after exiting the decider function from which the abandon was called.

> **Returns**

> This function returns no value.
</ins>


### Modifications in clause 7.25.5.1

#### In 7.25.5.1: The `abort` function

> The `abort` function causes abnormal program termination to occur, unless the signal `SIGABRT` is being caught and the signal handler does not return. Whether open streams with unwritten buffered data are flushed, open streams are closed, or temporary files are removed is implementation-defined. An implementation-defined form of the status *unsuccessful termination* is returned to the host environment by means of the function call `raise(SIGABRT)`.
>
> <ins>Calling this function is async-signal-safe.</ins>

### Modifications in clause 7.25.5.5

#### In 7.25.5.5: The `_Exit` function

> The `_Exit` function causes normal program termination to occur and control to be returned to the host environment. No functions registered by the `atexit` function, the `at_quick_exit` function, or signal handlers registered by the `signal` function are called. The status returned to the host environment is determined in the same way as for the `exit` function (7.25.5.4). Whether open streams with unwritten buffered data are flushed, open streams are closed, or temporary files are removed is implementation-defined.
>
> <ins>Calling this function is async-signal-safe.</ins>

### Modifications in clause 7.25.5.7

#### In 7.25.5.7: The `quick_exit` function

> The `quick_exit` function causes normal program termination to occur. No functions registered by the `atexit` function or signal handlers registered by the signal function are called. If a program calls the `quick_exit` function more than once, or calls the `exit` function in addition to the `quick_exit` function, the behavior is undefined. If a signal is raised while the `quick_exit` function is executing, the behavior is undefined.
>
> <ins>Calling this function is async-signal-safe.</ins>


### Modifications in clause 7.30 Threads `<threads.h>`

#### In 7.30.1 Introduction

**In paragraph 4, after the entry for `tss_t`, insert:**

<ins>
> `tss_async_signal_safe_t`
> which is a complete object type that holds an identifier for an async-signal-safe thread-specific storage pointer.

> The `tss_async_signal_safe_attr` structure shall contain at least the following members, in any order. The semantics of the members are expressed in the comments.
>
> ```
> // Create an instance
> int (*create)(void **dest);
>
> // Destroy an instance
> int (*destroy)(void *v);
> ```
>
> The functions pointed to by the members of this structure shall be thread-safe and reentrant.

> <ins>
> EXAMPLE 1: Use `tss_async_signal_safe_get` to retrieve the thread-specific storage pointer in a signal handler:
> </ins>

> <ins>
> ```
> struct thread_state
> {
>   int signal_count;
>   ...
> };
>
> static int create_state(void **dest)
> {
>   struct thread_state *state = malloc(sizeof(*state));
>   if(state == nullptr)
>   {
>     return -1;
>   }
>   *dest = state;
>   return 0;
> }
>
> static int destroy_state(void *v)
> {
>   free(v);
>   return 0;
> }
>
> static const struct tss_async_signal_safe_attr attr = {
>   .create = create_state,
>   .destroy = destroy_state,
> };
>
> static tss_async_signal_safe_t tss;
>
> /* Called near the beginning of each thread */
> static void thread_setup(void)
> {
>   if(tss_async_signal_safe_create(&tss, &attr) == thrd_error ||
>      tss_async_signal_safe_thread_init(tss) == thrd_error)
>   {
>     abort();
>   }
> }
>
> /* Called from within a signal handler or signal decider */
> static void signal_handler(int signo)
> {
>   struct thread_state *state = tss_async_signal_safe_get(tss);
>   state->signal_count++;
>   ...
> }
> ```
> </ins>

</ins>

#### In 7.30.6.5: The `tss_async_signal_safe_create` function

<ins>
> **Synopsis**

> ```
> #include <threads.h>
> int tss_async_signal_safe_create(tss_async_signal_safe_t *val,
>                                  const struct tss_async_signal_safe_attr *attr);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on `*val`.

> Creates an async-signal-safe thread-specific storage pointer. A copy of `attr` is taken; the copy contains function pointers that are later called to create and destroy instances of the thread-specific storage. The object pointed to by `val` is set to a value that uniquely identifies the newly created instance.

> **Returns**

> This function returns `thrd_success` if successful and `thrd_error` if unsuccessful.
</ins>


#### In 7.30.6.6: The `tss_async_signal_safe_destroy` function

<ins>
> **Synopsis**

> ```
> #include <threads.h>
> int tss_async_signal_safe_destroy(tss_async_signal_safe_t val);
> ```

> **Description**

> Calling this function is thread-safe apart from other operations concurrently acting on the same instance.

> Destroys a previously created async-signal-safe thread-specific storage pointer. All thread-specific storage pointers associated with this instance are destroyed using the original `attr->destroy()` upon the successful return of this function.

> It is undefined behavior if this function is called while any thread that has called `tss_async_signal_safe_thread_init()` for this instance has not yet exited.

> **Returns**

> This function returns `thrd_success` if successful and `thrd_error` if unsuccessful.
</ins>

#### In 7.30.6.7: The `tss_async_signal_safe_thread_init` function

<ins>
> **Synopsis**

> ```
> #include <threads.h>
> int tss_async_signal_safe_thread_init(tss_async_signal_safe_t val);
> ```

> **Description**

> Calling this function is thread-safe.

> Creates the thread-specific storage pointer for the calling thread by invoking the original `attr->create()`. It is permitted to call this function multiple times on the same thread; subsequent calls have no effect.

> It is implementation-defined whether the thread-specific storage pointer created has the original `attr->destroy()` called on it at thread exit, when that occurs before the call to `tss_async_signal_safe_destroy()`.

> **Returns**

> This function returns `thrd_success` if successful and `thrd_error` if unsuccessful.
</ins>

#### In 7.30.6.8: The `tss_async_signal_safe_get` function

<ins>
> **Synopsis**

> ```
> #include <threads.h>
> void *tss_async_signal_safe_get(tss_async_signal_safe_t val);
> ```

> **Description**

> Calling this function is thread-safe and async-signal-safe.

> `tss_async_signal_safe_thread_init()` shall have been called on the same thread beforehand, in which case the thread-specific storage pointer created at that time is returned; otherwise the behavior is undefined.

> **Returns**

> This function returns the thread-specific storage pointer for the calling thread.
</ins>

[N2471]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2471.pdf
[N2519]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2519.pdf
[N3540]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3540.pdf
[N3765]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3765.pdf
[N3783]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3783.pdf
[N3815]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3815.pdf
[N3856]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3856.pdf
[N3872]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3872.pdf
