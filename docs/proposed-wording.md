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
> 4. The object pointed to by the `void *` returned by `tss_async_signal_safe_get()`.
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

> **Insert the following new paragraphs after paragraph 5:**

> <ins>
> At a call of the `sigfence` macro (7.14.1), for the memory that the call describes: the value of that memory at the point of the call is the value most recently stored to that memory before the call; an access to that memory that is sequenced after the call reads the memory; and no access to that memory is performed on the other side of the call from where it is sequenced.
> </ins>

> <ins>
> A *non-local jump* is a transfer of control that restores a calling environment saved by an earlier operation and resumes thread execution at the point of that operation. A non-local jump is performed by the `longjmp` function (7.13.3.1), and by other functions and macros of the standard library explicitly described as performing a non-local jump; such functions and macros are not subject to the restrictions of 7.13.3.1. The environment saved by such an operation consists of information sufficient to return execution to the correct block and invocation of that block, were it called recursively; it does not include the state of the floating-point environment, of open files, or of any other component of the abstract machine.
> </ins>

> <ins>
> When a non-local jump is performed, all accessible objects have values, and all other components of the abstract machine have state, as of the time the non-local jump was performed, except that the representation of objects of automatic storage duration that are local to the function containing the operation that saved the environment, that do not have volatile-qualified type, and that have been changed between the saving of the environment and the non-local jump, is indeterminate. The representation of an object modified before a call to the `sigfence` macro (7.14.1) whose memory that call describes is not indeterminate.
> </ins>

### Modifications in clause 7.13 Non-local jumps `<setjmp.h>`

#### In 7.13.1 General

> **Paragraph 3.** The type declared is
>
> ```
> jmp_buf
> ```
>
> which is an array type suitable for holding the information needed to restore a calling environment. <del>The environment of an invocation of the setjmp macro consists of information sufficient for a call to the longjmp function to return execution to the correct block and invocation of that block, were it called recursively. It does not include the state of the floating-point environment, of open files, or of any other component of the abstract machine.</del>

#### In 7.13.2.1: The `setjmp` macro

> **Description**

> The `setjmp` macro saves its calling environment in its `jmp_buf` argument <del>for later use by the `longjmp` function</del><ins>for later restoration by a non-local jump (5.2.2.4)</ins>.

> **Returns**

> If the return is from a direct invocation, the `setjmp` macro returns the value zero. If the return is from <del>a call to the `longjmp` function</del><ins>a non-local jump (5.2.2.4)</ins>, the `setjmp` macro returns a nonzero value.

#### In 7.13.3.1: The `longjmp` function

> **Description**

> The `longjmp` function <del>restores the environment</del><ins>performs a non-local jump (5.2.2.4) to the environment</ins> saved by the most recent invocation of the `setjmp` macro in the same invocation of the program with the corresponding `jmp_buf` argument. If there has been no such invocation, or if the invocation was from another thread of execution, or if the function containing the invocation of the `setjmp` macro has terminated execution in the interim, or if the invocation of the `setjmp` macro was within the scope of an identifier with variably modified type and execution has left that scope in the interim, the behavior is undefined.

> <del>All accessible objects have values, and all other components of the abstract machine have state, as of the time the longjmp function was called, except that the representation of objects of automatic storage duration that are local to the function containing the invocation of the corresponding setjmp macro that do not have volatile-qualified type and have been changed between the setjmp invocation and longjmp call is indeterminate.</del>

> **Returns**

> After `longjmp` is completed, thread execution continues as if the corresponding invocation of the `setjmp` macro had just returned the value specified by `val`<ins> (5.2.2.4)</ins>. The `longjmp` function cannot cause the `setjmp` macro to return the value 0; if `val` is 0, the `setjmp` macro returns the value 1.

### Modifications in clause 7.14.1

> **Paragraph 1.** The header `<signal.h>` declares <del>a type and two functions and
> defines several macros</del> <ins>types, functions and macros</ins>,
> for handling various *signals* (conditions that may be reported
> during program execution).

> **Insert the following new paragraphs after paragraph 1:**

> <ins>
> A signal can be received by a thread within the program.
> When a signal is received, execution is interrupted and the signal
> is handled as described below.
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
> *Async-signal-safe* functions and macros are those safe to call during the handling of a signal. Only the functions and macros in the standard library listed below are required to be async-signal-safe; all other functions and macros in the standard library need not be async-signal-safe. This document does not specify whether functions of the program that are not in the standard library are async-signal-safe. The following functions and macros are required to be async-signal-safe:
>
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument,
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the signal handler or signal decider. Furthermore, if such a call to the signal function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation, or
> - any function or macro within this standard explicitly described as async-signal-safe.
>
> There are two ways to specify the handling of a signal:
>
> 1. The `signal` function globally installs a single signal
> handler for the whole program execution, overwriting any handler previously installed by the `signal` function.
>
> 2. The `siginstall` function enables an alternative
> signal handling mechanism which implements thread-safe composable signal handling.
>
> Signal handling state persists for the whole program execution and consists of the following kinds, all of which coexist:
>
> - the signal handler installed by the `signal` function (7.14.2.3): a single handler per signal number, replaced by each call of the `signal` function, and which persists whether or not the thread-safe implementation is activated for the signal number;
> - the thread-local signal deciders installed by the `sigguarded` function (7.14.3.1), one ordered sequence per thread, which persist whether or not the thread-safe implementation is activated for the signal numbers of their signal sets;
> - the global signal deciders installed by the `signal_decider_create` function (7.14.2.7), which persist whether or not the thread-safe implementation is activated for the signal numbers of their signal sets; and
> - the activation count for each signal number: the number of calls of `siginstall` that install that signal number, minus the number of calls of `siguninstall` that uninstall it; the thread-safe implementation is activated for a signal number while its activation count is nonzero.
>
> The kinds of state coexist whether or not the thread-safe implementation is activated for a signal number: activation and deactivation create or remove none of the other kinds of state. In particular, a call of the `signal` function replaces only the signal handler installed by a previous call of the `signal` function; it does not remove thread-local or global signal deciders, nor does it change the activation count for any signal number. For the purposes of this subclause, the equivalent of `signal(sig, SIG_DFL)` that an implementation may perform prior to calling a signal handler (7.14.2.3) is considered a call of the `signal` function.
> </ins>

> <ins>
> If the thread-safe implementation is not activated for the signal number of the signal received, then the following sequence occurs on signal receipt:
>
> 1. If there is such a handler, the most recently installed handler by `signal` for that signal number is called, unless that handler was set to `SIG_IGN`, in which case the signal is ignored, or was set to `SIG_DFL`, in which case the default action for that signal number on that implementation is performed.
> The thread in which the handler is called is unspecified.
> 2. Otherwise, if no call of `signal` for the signal number was performed, the handler has `SIG_DFL` semantics, which is the default action for that signal number on that implementation.
>
> If the thread-safe implementation is activated for the signal number of the signal received, then the following sequence occurs on signal receipt:
>
> 1. For synchronous category signals, the signal deciders shall be invoked on the thread that caused the signal. For asynchronous category signals, the thread on which the signal deciders are invoked is unspecified.
>
> 2. An ordered sequence of signal deciders is invoked on the thread that received the signal to decide how to handle the signal. The ordered sequence begins with the thread-locally installed signal deciders whose signal set matches the signal number, in order of most recently installed first for that thread, followed by the globally installed signal deciders whose signal set matches the signal number:
>     - For thread-locally installed signal deciders, each decider function is called with a pointer to a valid `stdc_siginfo`, with its `value` member set to the value that was specified when that decider was installed. If a decider function returns:
>         - `sig_decision_resume_execution`: execution of the interrupted thread is resumed.
>         - `sig_decision_call_recovery`: a non-local jump (5.2.2.4) is performed to the calling environment saved when that thread-local decider was installed, and the recovery function as specified at that time shall be called to implement recovery from the signal raise for that thread.
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
> It is implementation-defined whether, when a signal occurs while an earlier occurrence of the same signal has occurred but has not yet been completely processed, the later occurrence is blocked until the processing of the earlier occurrence has completed, is discarded, or is processed immediately, in which case its processing can be nested within that of the earlier occurrence. In particular, an implementation may block the signal during the execution of a signal handler or of signal deciders for that signal, and may discard occurrences of a signal that occurs repeatedly before its processing completes, so that only one occurrence is processed.
>
> It is permitted for a signal decider to never return. Signal deciders shall meet the requirements of 7.14.4.
> A signal decider that determines that it will never return through the signal decider machinery should call `sigdecider_abandon()` before not returning, and may call `sigdecider_abandon_resume()` to retract that declaration if it later determines that it will return after all (7.14.3.2 and 7.14.3.3).
> If every signal decider returns `sig_decision_next_decider`, the behavior is implementation-defined. While the thread-safe implementation is activated for a signal number, the signal handler installed by the `signal` function for that signal number is not used to handle signals with that number, other than as part of the implementation-defined behavior of the previous sentence. Conversely, while the thread-safe implementation is not activated for a signal number, signal deciders are not used to handle signals with that number.
>
> `siginstall` may be called multiple times, and for each a corresponding `siguninstall` should be present in the program. The activation count for a signal number is incremented by each call of `siginstall` that installs that signal number, and is decremented by each call of `siguninstall` that uninstalls it; the thread-safe implementation shall not be deactivated for a signal number until the last uninstallation for that signal number is performed, that is, until its activation count reaches zero. While the activation count for a signal number is zero, a signal with that number is handled as specified for a signal for which the thread-safe implementation has not been activated, and no signal deciders are invoked for it; when the activation count for a signal number becomes nonzero again, the signal deciders whose signal sets include that signal number are invoked again. Activation and deactivation of the thread-safe implementation do not install or remove signal deciders or signal handlers.
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
> which is an implementation-defined possibly incomplete object type that represents the system-specific signal information.
> </ins>

> <ins>
> `stdc_siginfo_context_t`
> which is an implementation-defined possibly incomplete object type that represents the system-specific context.
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
> // thread-local signal deciders only: restore the environment
> // to what it was at entry to `sigguarded()`, and call the
> // recovery function.
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
> which is an implementation-defined complete object type able to represent a set of signals on this platform. Copying the representation of a `sigset_t` object, by assignment or otherwise, yields an object that represents the same set of signals as the original; the set represented does not depend on the address of the object or on the continued existence of the object from which it was copied. In particular, the following code is valid:
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
> and the following macro, which restricts, for the memory described below, the freedom of an implementation to deviate from the abstract machine (5.2.2.4):

> `sigfence(vars ...)` provides the guarantees specified in 5.2.2.4 for the following memory:

> - the memory storing all objects with external or internal linkage;
> - the memory storing the objects without linkage named by `vars ...`.

> `sigfence()` is *async-signal-safe*. The macro accepts between zero and eight arguments; any additional arguments cause a diagnostic. Each argument, if any, shall be an lvalue designating an object without linkage.

> NOTE: `atomic_signal_fence()` provides weaker guarantees than `sigfence()`, and may be sufficient for some performance-oriented use cases.
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
new sections "7.14.3 Recover from signal" and "7.14.4 Requirements for
signal handlers and signal deciders" are added after clause 7.14.2.

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

> 5 <del>If the signal occurs other than as the result of calling the `abort` or `raise` function, the behavior is undefined if the signal handler refers to any object with static or thread storage duration that is not a lock-free atomic object and that is not declared with the `constexpr` storage-class specifier other than by assigning a value to an object declared as `volatile sig_atomic_t`, or the signal handler calls any function in the standard library other than
> - the `abort` function,
> - the `_Exit` function,
> - the `quick_exit` function,
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument, or
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the handler. Furthermore, if such a call to the signal function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation.
> </del><ins>The signal handler shall meet the requirements of 7.14.4.</ins>

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

> For all signals in the signal set originally installed by the `siginstall()` call that returned `handle`, the thread-safe implementation shall be deactivated according to the Introduction above. Uninstallation does not remove any signal deciders, and no signal decider is associated with any particular call of `siginstall` or `siguninstall`: while the thread-safe implementation is deactivated for a signal number, signals with that number are handled as specified in the Introduction for signals for which the thread-safe implementation has not been activated, and signal deciders are not invoked for them, until the thread-safe implementation is activated again for that signal number.

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

> Installs a global signal continuation decider function, which shall meet the requirements of 7.14.4. See Introduction for how global signal continuation decider functions are called.

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

> Signal deciders are invoked for `signo` only if the thread-safe implementation is activated for `signo`. Otherwise, this function behaves as if it called the `raise` function without invoking any signal deciders.

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

> Calling this function is thread-safe and async-signal-safe. The behavior is undefined if this function is called during the handling of a signal for which no call to `siginstall` with a signal set containing that signal number has been performed in the current program execution. The `decider` function shall meet the requirements of 7.14.4.

> Installs a thread-local signal continuation decider function, saving the calling environment such that it can be restored later by a non-local jump (5.2.2.4). If a decider installed by this call returns `sig_decision_call_recovery`, a non-local jump (5.2.2.4) to the saved calling environment is performed, and the `recovery` function is called to implement recovery from the signal raise. See 7.14.1 for how thread-local signal continuation decider functions are called.

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

### Add 7.14.4 Requirements for signal handlers and signal deciders

<ins>
> The requirements of this subclause apply to a signal handler (7.14.2.3) and to a signal decider (7.14.2.7, 7.14.3.1).
>
> When the signal occurs other than as the result of calling the `abort` or `raise` function, the signal handler or signal decider shall not call any function in the standard library not described as async-signal-safe (7.14.1).
>
> The rules of 5.2.2.4 apply to any object modified by a signal handler or signal decider.
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
> which is a complete object type that holds an identifier for an async-signal-safe thread-specific storage pointer. The identifier is the value set by `tss_async_signal_safe_create`, and is not the thread-specific storage pointer itself.

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

> Creates an async-signal-safe thread-specific storage pointer. A copy of `attr` is taken; the copy contains function pointers that are later called to create and destroy instances of the thread-specific storage. The object pointed to by `val` is set to a value that uniquely identifies the newly created instance. Each instance has its own thread-specific storage pointer for each thread, created by `tss_async_signal_safe_thread_init` (7.30.6.7).

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

> Creates the thread-specific storage pointer for the calling thread for the instance identified by `val`, by invoking the original `attr->create()`. It is permitted to call this function multiple times on the same thread for the same instance; subsequent calls do not invoke the original `attr->create()` again, do not change the thread-specific storage pointer, and return `thrd_success`.

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

> `tss_async_signal_safe_thread_init()` shall have been called on the same thread for the instance identified by `val` beforehand, in which case the thread-specific storage pointer created at that time for that instance is returned; otherwise the behavior is undefined.

> **Returns**

> This function returns the thread-specific storage pointer for the calling thread for the instance identified by `val`.
</ins>

[N2471]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2471.pdf
[N2519]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2519.pdf
[N3540]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3540.pdf
[N3765]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3765.pdf
[N3783]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3783.pdf
[N3815]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3815.pdf
[N3856]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3856.pdf
[N3872]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3872.pdf
