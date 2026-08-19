---
title: "Nxxxx: Wording for \"Thread-safe signals handling rev 5\""
author:
    - Douglas, Niall
date:
    - 2026-08-19
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
| [N3924](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3924.htm) | Thread-safe signals handling           | Douglas, Niall | revision 4, 2026-06-26    |
| [N3886](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3886.pdf) | Working Draft                          |                | base for diff, 2026-05-24 |
| Nxxxx                                                                 | &lt;this paper&gt;                     | Wording Group  |                           |


### LaTeX document branch

none

### Liaison

WG21, Austin Group

### Relevant polls

| meeting                                       | date       | for | against | abstain |
|-----------------------------------------------|------------|-----|---------|---------|
| Nxxxx revision 5                              | N/A        | N/A | N/A     | N/A     |
| N3924 revision 4                              | N/A        | N/A | N/A     | N/A     |
| N3872 revision 3                              | N/A        | N/A | N/A     | N/A     |
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


### Add a definition to clause 3 Terms, definitions, and symbols

Insert the following new entry between 3.22 (recommended practice) and 3.23
(runtime-constraint); the new entry is numbered 3.23, and the entries 3.23
through 3.33 are renumbered 3.24 through 3.34:

<ins>
> **3.23 reentrant**
>
> reentrant
> able to be entered or invoked again before a previous entry or invocation has completed, without interference between the entries or invocations.
>
> Note 1 to entry: Reentrancy does not by itself imply that calls can be executed concurrently by multiple threads; that property is addressed by the data race rules (5.2.2.5).
</ins>

### Modifications in clause 5.2.2.4 paragraph 6

> When the processing of the abstract machine is interrupted by receipt of a signal, the values of objects
> that are <del>neither lock-free atomic objects nor of type `volatile sig_atomic_t`</del>

> <ins>
> not in any of the following categories:
>
> 1. A lock-free atomic object.
> 2. An object of type `volatile sig_atomic_t`.
> 3. An object not modified after a call to the `sigfence` macro (7.14.1) that describes the memory of the object and that is sequenced before the interruption.
> 4. An object pointed to by the `void *` value returned by `tss_async_signal_safe_get` (7.30.6.8).
> </ins>

> are unspecified, as is
> the state of the dynamic floating-point environment. The representation of any object modified by
> the handler that is <del>neither a lock-free atomic object nor of type `volatile sig_atomic_t`</del>

> <ins>not in any of the following categories:
>
> 1. A lock-free atomic object.
> 2. An object of type `volatile sig_atomic_t`.
> 3. An object not modified after a call to the `sigfence` macro (7.14.1) that describes the memory of the object and that is sequenced before the handler exits.
> 4. An object pointed to by the `void *` value returned by `tss_async_signal_safe_get` (7.30.6.8).
> </ins>

> becomes
> indeterminate when the handler exits, as does the state of the dynamic floating-point environment if
> it is modified by the handler and not restored to its original state. <ins>For the purposes of this
> paragraph, the processing of the abstract machine is interrupted when the handling of the signal
> (7.14.1) begins.
</ins>

> **Insert the following new paragraphs after paragraph 6; the existing paragraphs 7 through 17 are renumbered 11 through 21:**

> <ins>
> 7 At a call to the `sigfence` macro (7.14.1), the implementation shall not deviate from the abstract machine for the memory that the call describes: the value of the memory at the point of the call is the value most recently stored to the memory before the call; an access to the memory that is sequenced after the call reads from the memory; and no access to the memory is performed on the other side of the call from where it is sequenced.
> </ins>

> <ins>
> 8 NOTE The two category 3 restrictions differ: the first requires the call of the `sigfence` macro to be sequenced before the interruption, because only then is the value of the memory at the interruption guaranteed; the second has no such requirement: a signal handler or signal decider can instead preserve the representation of an object it modifies by calling the `sigfence` macro during the handling of the signal (7.14.1), after the modification.
> </ins>

> <ins>
> 9 A *non-local jump* is a transfer of control that restores a calling environment saved by an earlier operation and resumes thread execution at the point of that operation. A non-local jump is performed by the `longjmp` function (7.13.3.1), and by the signal recovery mechanism specified in 7.14.3.1; the restrictions of 7.13.3.1 do not apply to a non-local jump performed by signal recovery. The environment saved by such an operation consists of information sufficient to return execution to the correct block and invocation of that block, were it called recursively; it does not include the state of the floating-point environment, of open files, or of any other component of the abstract machine.
> </ins>

> <ins>
> 10 When a non-local jump is performed, all accessible objects have values, and all other components of the abstract machine have state, as of the time the non-local jump was performed, except that the representation of objects of automatic storage duration that are local to the function containing the operation that saved the environment and that do not have volatile-qualified type and have been changed between the saving of the environment and the non-local jump is indeterminate. The representation of an object that is not modified after a call to the `sigfence` macro (7.14.1) that describes the memory of the object and that is sequenced before the non-local jump is not indeterminate.
> </ins>

> **In the forward references of 5.2.2.4, replace "the signal function (7.14), files (7.24.3)" with "non-local jumps (7.13), the sigfence macro (7.14.1), the signal function (7.14.2.3), the signal recovery mechanism (7.14.3.1), files (7.24.3), the tss_async_signal_safe_get function (7.30.6.8)":**

### Modifications in clause 7.13 Non-local jumps `<setjmp.h>`

#### In 7.13.1 General

> 3 The type declared is
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

> 1 The header `<signal.h>` declares <del>a type and two functions and
> defines several macros</del> <ins>types, enumeration constants, and functions, and defines macros</ins>,
> for handling various *signals* (conditions that may be reported
> during program execution).

> **Insert the following new paragraphs after paragraph 1:**

> <ins>
> 2 A signal can be received by a thread within the program.
> When a signal is received, the processing of the abstract machine is
> interrupted (5.2.2.4) and the signal is handled as described below.
>
> 3 There are the following categories of signal:
> </ins>

> <ins>
> 1. *Synchronous:* these are caused by the execution of a thread. Standard
> signals are:
>
>     - abnormal termination,
>     - erroneous arithmetic operation,
>     - detection of an invalid function image, and
>     - invalid access to storage.
>
>     The implementation may define additional signals in the synchronous category.
>
> 2. *Asynchronous non-debug:* these are generated by the environment and are not usually generated by testing. Standard signals are:
>
>     - receipt of an interactive attention signal, and
>     - termination request sent to the program.
>
>     The implementation may define additional signals in the asynchronous non-debug category.
>
> 3. *Asynchronous debug:* these are generated by the environment and
>    are usually generated by testing.
>
>     The implementation may define additional signals in the asynchronous debug category.
>
> 4 *Async-signal-safe* functions and macros are those safe to call during the handling of a signal. Only the functions and macros in the standard library listed below are required to be async-signal-safe; all other functions and macros in the standard library need not be async-signal-safe. This document does not specify whether functions of the program that are not in the standard library are async-signal-safe. The following functions and macros are required to be async-signal-safe:
>
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument,
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the signal handler, or
> - any function or macro in this document explicitly described as async-signal-safe.
>
> If such a call to the `signal` function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation.
>
> 5 There are two ways to specify the handling of a signal:
>
> 1. The `signal` function installs a single signal handler for the whole program, replacing any handler previously installed by the `signal` function.
>
> 2. The `siginstall` function enables an alternative, thread-safe, composable signal handling mechanism.
>
> 6 A *signal decider* is a function of type `sig_decide_t` that is installed by a call of the `sigguarded` function (7.14.3.1) or of the `signal_decider_create` function (7.14.2.7) and that is invoked during the handling of a signal to decide how that signal is to be handled. Signal deciders installed by the `sigguarded` function are *thread-local signal deciders*; signal deciders installed by the `signal_decider_create` function are *global signal deciders*.
>
> 7 Signal handling state persists for the entire execution of the program and consists of the following kinds, all of which coexist:
>
> - the signal handler installed by the `signal` function (7.14.2.3): a single handler per signal number, replaced by each call of the `signal` function, which persists whether or not the signal number is activated;
> - the thread-local signal deciders installed by the `sigguarded` function (7.14.3.1), one ordered sequence per thread, which persist whether or not the signal numbers in their signal sets are activated;
> - the global signal deciders installed by the `signal_decider_create` function (7.14.2.7), which persist whether or not the signal numbers in their signal sets are activated; and
> - the activation count for each signal number: the number of calls of `siginstall` that install that signal number, minus the number of calls of `siguninstall` that uninstall it; a signal number is *activated* while its activation count is nonzero, and is *deactivated* while its activation count is zero.
>
> 8 The kinds of state coexist whether or not a signal number is activated: activation and deactivation create or remove none of the other kinds of state. In particular, a call of the `signal` function replaces only the signal handler installed by a previous call of the `signal` function; it does not remove thread-local or global signal deciders, nor does it change the activation count for any signal number. For the purposes of this subclause, the equivalent of `signal(sig, SIG_DFL)` that an implementation may perform prior to calling a signal handler (7.14.2.3), and the equivalents of `signal(sig, SIG_IGN)` and `signal(sig, SIG_DFL)` that an implementation may perform at program startup (7.14.2.3), are considered calls of the `signal` function.
>
> 9 Concurrent calls of the `siginstall` (7.14.2.5), `siguninstall` (7.14.2.6), `signal_decider_create` (7.14.2.7), and `signal_decider_destroy` (7.14.2.8) functions from multiple threads shall not introduce data races.
> </ins>

> <ins>
> 10 If the signal number of the signal received is not activated, then the following sequence occurs on receipt of the signal:
>
> 1. The handler installed by the most recent call of the `signal` function for that signal number before the receipt of the signal is called, unless that handler is `SIG_IGN`, in which case the signal is ignored, or is `SIG_DFL`, in which case the default handling for that signal number on that implementation is performed.
> The thread on which the handler is called is unspecified.
>
> 11 If the signal number of the signal received is activated, then the following sequence occurs on receipt of the signal:
>
> 1. For synchronous category signals, the signal deciders are invoked on the thread that caused the signal; for asynchronous category signals, the thread on which the signal deciders are invoked is unspecified.
>
> 2. An ordered sequence of signal deciders is invoked on that thread to decide how to handle the signal. The ordered sequence begins with the thread-local signal deciders whose signal sets include the signal number, most recently installed first, followed by the global signal deciders whose signal sets include the signal number. The ordered sequence is determined when the signal is received: a thread-local signal decider installed while an earlier occurrence of a signal is being processed is not invoked for that occurrence, but is invoked for occurrences received after its installation. Each decider is called with a pointer to a `struct stdc_siginfo` object whose `signo` member is set to the number of the signal received and whose `value` member is set to the `value` argument of the call of `sigguarded` (7.14.3.1) or `signal_decider_create` (7.14.2.7) that installed the decider. For a signal other than one raised by the `stdc_raise` function (7.14.2.9), the values of the remaining members of the `struct stdc_siginfo` object are implementation-defined, except that the `raw_info` and `raw_context` members may be null pointers. The `struct stdc_siginfo` object passed to a signal decider is not subject to the rules of 5.2.2.4 for objects modified by a signal handler or by a signal decider, and its lifetime extends until the handling of the signal completes; if the recovery function of the `sigguarded` function call (7.14.3.1) that installed the decider is called, its lifetime extends instead until the recovery function returns.
>     - For thread-local signal deciders, if a decider function returns:
>         - `sig_decision_resume_execution`: execution of the interrupted thread is resumed.
>         - `sig_decision_call_recovery`: a non-local jump (5.2.2.4) is performed to the calling environment saved when that thread-local decider was installed, and the recovery function specified at that time shall be called, with a pointer to the `struct stdc_siginfo` object that was passed to the decider, to implement recovery from the signal raise for that thread.
>         - `sig_decision_next_decider`: the next decider in the sequence is called.<br><br>
>
>     - For global signal deciders, the deciders are called in the following order: first, those installed with the `callfirst` argument true, most recently installed first; then, those installed with the `callfirst` argument false, in the order in which they were installed.
>
>         If a decider function returns:
>
>         - `sig_decision_resume_execution`: execution of the interrupted thread is resumed.
>         - `sig_decision_call_recovery`: the default handling for that signal number on that implementation is performed.
>         - `sig_decision_next_decider`: the next decider in the sequence is called.
>
> 12 It is implementation-defined whether, when a signal occurs while an earlier occurrence of the same signal has not yet been completely processed, the later occurrence is blocked until the processing of the earlier occurrence has completed, is discarded, or is processed immediately, in which case its processing may be nested within that of the earlier occurrence. In particular, an implementation may block the signal during the execution of a signal handler or of signal deciders for that signal, and may discard repeated occurrences before the processing of the signal completes, so that only one occurrence is processed.
>
> 13 A signal decider is not required to return. Signal deciders shall meet the requirements of 7.14.4.
> A signal decider that determines that it will never return should call the `sigdecider_abandon` function (7.14.3.2) before it does so, and may call the `sigdecider_abandon_resume` function (7.14.3.3) to retract that declaration if it later determines that it will return.
> 14 If every signal decider in the ordered sequence returns `sig_decision_next_decider`, or if the ordered sequence is empty, the default handling for that signal number on that implementation is performed. While a signal number is activated, the signal handler installed by the `signal` function for that signal number is not used to handle signals with that number. Conversely, while a signal number is not activated, signal deciders are not used to handle signals with that number.
>
> 15 The activation count for a signal number is incremented by each call of `siginstall` that installs that signal number, and is decremented by each call of `siguninstall` that uninstalls it; a signal number shall not be deactivated until the last uninstallation for that signal number is performed, that is, until its activation count reaches zero. While a signal number is not activated, a signal with that number is handled as specified for a signal whose signal number is not activated, and no signal deciders are invoked for it; when a signal number becomes activated again, the signal deciders whose signal sets include that signal number are invoked again. Activation and deactivation of signal numbers do not install or remove signal deciders or signal handlers.
> </ins>

> <ins>
> **Recommended practice**
> 16 It is recommended practice
> that pre-existing programs be upgraded to use `siginstall`,
> and that newly written code prefer `siginstall`
> over `signal`.
> </ins>

> <ins>
> 17 EXAMPLE 1 Use `sigguarded` to recover from a `SIGFPE`
> </ins>

> <ins>
> ```
> #include <signal.h>
> #include <assert.h>
> #include <stdlib.h>
>
> /* Marker used to verify the value returned by recovery */
> static int sigfpe_marker;
>
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
>   if (rsi->signo != SIGFPE)
>   {
>     abort();
>   }
>   rsi->value.ptr_value = &sigfpe_marker;
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
>   union stdc_siginfo_value value = { .ptr_value = nullptr };
>   sigset_t guarded;
>   sigemptyset(&guarded);
>   sigaddset(&guarded, SIGFPE);
>   /* Activate SIGFPE */
>   siginstall(&guarded);
>   value = sigguarded(&guarded,
>                     sigfpe_func,
>                     sigfpe_recovery_func,
>                     sigfpe_decider_func,
>                     value);
>   assert(value.ptr_value == &sigfpe_marker);
> }
> ```
>
> 18 NOTE The integer division by zero in this example is undefined behavior (6.5.5); the example relies on the implementation trapping the operation and raising `SIGFPE`, and falls back to `stdc_raise` when it does not.
> </ins>

> 19 (base paragraph 2) The <del>type defined is</del><ins>types defined are</ins>

> <ins>
> `stdc_siginfo_error_code_t`
> which is an implementation-defined complete object type that represents a system-specific error code.
> </ins>


> <ins>
> The `stdc_siginfo_value` union shall contain at least a member
> `void *ptr_value`, and, if `intptr_t` is defined (7.23.2.5),
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
> // The memory location that caused the fault, if appropriate
> void *addr;
>
> // A user-defined value
> union stdc_siginfo_value value;
>
> // The system specific information. Can be a null pointer if the
> // information was not supplied, or was supplied as a null pointer.
> stdc_siginfo_siginfo_t *raw_info;
>
> // The system specific context. Can be a null pointer if the
> // information was not supplied, or was supplied as a null pointer.
> stdc_siginfo_context_t *raw_context;
> ```
> </ins>

> <ins>
> The `sig_func_t` type is a function type with a single argument of type
> `union stdc_siginfo_value` and a return type of `union stdc_siginfo_value`,
> used as the type of the guarded function of the `sigguarded` function (7.14.3.1).
> </ins>

> <ins>
> The `sig_recover_t` type is a function type with a single argument of type
> `const struct stdc_siginfo *` and a return type of `union stdc_siginfo_value`,
> used as the type of the recovery function of the `sigguarded` function (7.14.3.1).
> </ins>

> <ins>
> The `sig_decision` enumeration shall contain at least the following members, in any order. The semantics of the members are expressed in the comments:
> </ins>

> <ins>
> ```
> // Defer to the next decider
> sig_decision_next_decider
>
> // Resume execution; the cause of the signal has been fixed
> sig_decision_resume_execution
>
> // Thread-local signal deciders only: restore the environment
> // to what it was at entry to sigguarded(), and call the
> // recovery function
> sig_decision_call_recovery
> ```
> </ins>

> <ins>
> The `sig_decide_t` type is the function type of a signal decider (7.14.1):
> a function with a single argument of type `struct stdc_siginfo *` and a
> return type of `enum sig_decision`.
> </ins>

> `sig_atomic_t`

> which is the (possibly volatile-qualified) integer type of an object that can be accessed as an atomic entity, even in the presence of asynchronous interrupts.

> <ins>
> `sigset_t`
> </ins>

> <ins>
> which is an implementation-defined complete object type able to represent a set of signals. Copying the representation of a `sigset_t` object, by assignment or otherwise, yields an object that represents the same set of signals as the original; the set represented does not depend on the address of the object or on the continued existence of the object from which it was copied. In particular, the following code is valid:
> </ins>

> <ins>
> ```
> sigset_t a;
> sigemptyset(&a);
> sigset_t b = a;
> ```
> </ins>

> **After paragraph 19 (base paragraph 2), insert the following new paragraph:**

> <ins>
> 20 `SIGGUARDED_FAILURE_VALUE`, which is a `constexpr` object of type `union stdc_siginfo_value` with an implementation-defined value, is the value returned by the `sigguarded` function (7.14.3.1) when it fails to install the guard.
> </ins>

> 21 (base paragraph 3) The macros defined are

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

> **In paragraph 21 (base paragraph 3), insert the following after the `SIGTERM` entry:**

<ins>
> and the following macro, which restricts, for the memory described below, the freedom of an implementation to deviate from the abstract machine (5.2.2.4):

> `sigfence(...)` provides the guarantees specified in 5.2.2.4 for the following memory:

> - the memory storing all objects with external or internal linkage;
> - the memory storing the objects without linkage named by the arguments of the invocation.

> `sigfence()` is async-signal-safe. The behavior is undefined if the `sigfence` macro is invoked with more than eight arguments. Each argument, if any, shall be an lvalue designating an object; the behavior is undefined if an argument is a bit-field or designates an object declared with the `register` storage-class specifier. An argument that designates an object with external or internal linkage is permitted but has no effect, because the memory storing such objects is described by any invocation of this macro.
</ins>

> **After paragraph 21 (base paragraph 3), insert the following new paragraph:**

<ins>
> 22 NOTE The `atomic_signal_fence` function (7.17.4.3) provides weaker guarantees than the `sigfence` macro, and may be sufficient for performance-oriented use cases.
</ins>

> 23 (base paragraph 4) An implementation is not required to generate any of these signals, except as a result of explicit calls to the `raise` function. Additional signals and pointers to undeclarable functions, with macro definitions beginning, respectively, with the letters `SIG` and an uppercase letter or with `SIG_` and an uppercase letter, may also be specified by the implementation. The complete set of signals, their semantics, and their default handling is implementation-defined; all signal numbers shall be positive.

**After paragraph 23 (base paragraph 4), insert the following new paragraph:**

<ins>
> 24 A *valid signal number* is a positive integer that is the signal number of one of the signals defined by the implementation.
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

In the description of "The raise function" (7.14.2.4), insert "The `raise`
function is async-signal-safe." at the beginning of the description; change
"the actions described in 7.14.2.1" to "the actions described in 7.14.1";
and change "If a signal handler is called, the raise function shall not
return until after the signal handler does." to "If a signal handler is
called, the raise function shall not return until after the signal handler
does; if signal deciders are called, the raise function shall not return
until after the ordered sequence of signal deciders (7.14.1) completes.":

### 7.14.2.1 Signal set initialization

#### The `sigemptyset` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> int sigemptyset(sigset_t *setp);
> ```

> **Description**
> 2 The `sigemptyset` function is async-signal-safe.

> 3 The set of signals pointed to by `setp` is set to the empty set. It is permitted for the memory pointed to by `setp` to be uninitialized.

> **Returns**
> 4 The `sigemptyset` function always returns zero.
</ins>

#### The `sigfillset` function

<ins>
> **Synopsis**
> 5
> ```
> #include <signal.h>
> int sigfillset(sigset_t *setp);
> ```

> **Description**
> 6 The `sigfillset` function is async-signal-safe.

> 7 The set of signals pointed to by `setp` is set to the full set, the set of all signals defined by the implementation. It is permitted for the memory pointed to by `setp` to be uninitialized.

> **Returns**
> 8 The `sigfillset` function always returns zero.
</ins>

#### The `sigfillset_synchronous` function

<ins>
> **Synopsis**
> 9
> ```
> #include <signal.h>
> int sigfillset_synchronous(sigset_t *setp);
> ```

> **Description**
> 10 The `sigfillset_synchronous` function is async-signal-safe.

> 11 The set of signals pointed to by `setp` is set to exactly the set of synchronous signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> 12 Synchronous signals are those caused by the execution of a thread (7.14.1). This set can include additional implementation-defined signals; however, at least the following standard signals are in this set: `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGSEGV`.

> **Returns**
> 13 The `sigfillset_synchronous` function always returns zero.
</ins>

#### The `sigfillset_asynchronous_nondebug` function

<ins>
> **Synopsis**
> 14
> ```
> #include <signal.h>
> int sigfillset_asynchronous_nondebug(sigset_t *setp);
> ```

> **Description**
> 15 The `sigfillset_asynchronous_nondebug` function is async-signal-safe.

> 16 The set of signals pointed to by `setp` is set to exactly the set of asynchronous non-debug signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> 17 Asynchronous non-debug signals are those generated by the environment and not usually generated by testing (7.14.1). This set can include additional implementation-defined signals; however, at least the following standard signals are in this set: `SIGINT`, `SIGTERM`.

> **Returns**
> 18 The `sigfillset_asynchronous_nondebug` function always returns zero.
</ins>

#### The `sigfillset_asynchronous_debug` function

<ins>
> **Synopsis**
> 19
> ```
> #include <signal.h>
> int sigfillset_asynchronous_debug(sigset_t *setp);
> ```

> **Description**
> 20 The `sigfillset_asynchronous_debug` function is async-signal-safe.

> 21 The set of signals pointed to by `setp` is set to exactly the set of asynchronous debug signals. It is permitted for the memory pointed to by `setp` to be uninitialized.

> 22 Asynchronous debug signals are those generated by the environment and usually generated by testing (7.14.1).

> **Returns**
> 23 The `sigfillset_asynchronous_debug` function always returns zero.
</ins>

### 7.14.2.2 Signal set manipulation

#### The `sigaddset` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> int sigaddset(sigset_t *setp, int signo);
> ```

> **Description**
> 2 The `sigaddset` function is async-signal-safe.

> 3 Signal number `signo` is added to the set of signals pointed to by `setp`; if `signo` is already a member of the set, nothing is done. If `signo` is not a valid signal number (7.14.1), the set is not modified.

> 4 If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**
> 5 The `sigaddset` function returns zero if `signo` is a valid signal number (7.14.1) and a negative value otherwise.
</ins>

#### The `sigdelset` function

<ins>
> **Synopsis**
> 6
> ```
> #include <signal.h>
> int sigdelset(sigset_t *setp, int signo);
> ```

> **Description**
> 7 The `sigdelset` function is async-signal-safe.

> 8 Signal number `signo` is removed from the set of signals pointed to by `setp`; if `signo` is not a member of the set, nothing is done. If `signo` is not a valid signal number (7.14.1), the set is not modified.

> 9 If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**
> 10 The `sigdelset` function returns zero if `signo` is a valid signal number (7.14.1) and a negative value otherwise.
</ins>

#### The `sigismember` function

<ins>
> **Synopsis**
> 11
> ```
> #include <signal.h>
> int sigismember(const sigset_t *setp, int signo);
> ```

> **Description**
> 12 The `sigismember` function is async-signal-safe.

> 13 If `*setp` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> **Returns**
> 14 The `sigismember` function returns one if signal number `signo` is set within the set of signals pointed to by `setp`, zero if it is not set, and a negative value if `signo` is not a valid signal number (7.14.1).
</ins>

### Modifications in clause 7.14.2.3: The `signal` function

> ...

> **Description**

> ...

> 4 If the signal occurs as the result of calling <del>the `abort` or `raise` function</del><ins>the `abort`, `raise`, or `stdc_raise` function</ins>, the signal handler shall not call <del>the `raise` function</del><ins>the `raise` or `stdc_raise` function</ins>.

> 5 <del>If the signal occurs other than as the result of calling the `abort` or `raise` function, the behavior is undefined if the signal handler refers to any object with static or thread storage duration that is not a lock-free atomic object and that is not declared with the `constexpr` storage-class specifier other than by assigning a value to an object declared as `volatile sig_atomic_t`, or the signal handler calls any function in the standard library other than
> - the `abort` function,
> - the `_Exit` function,
> - the `quick_exit` function,
> - the functions in `<stdatomic.h>` (except where explicitly stated otherwise) when the atomic arguments are lock-free,
> - the `atomic_is_lock_free` function with any atomic argument, or
> - the `signal` function with the first argument equal to the signal number corresponding to the signal that caused the invocation of the handler. Furthermore, if such a call to the signal function results in a `SIG_ERR` return, the object designated by `errno` has an indeterminate representation.
> </del><ins>The signal handler shall meet the requirements of 7.14.4.</ins>

> 6 ...

> 7 Use of this function in a multi-threaded program results in undefined behavior. <ins>It is also undefined behavior to call this function for a signal number that is activated.</ins> <del>The implementation shall behave as if no library function calls the signal function.</del>

### Add the new signal handling functions to clause 7.14.2


#### Add 7.14.2.5: The `siginstall` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> void *siginstall(const sigset_t *guarded);
> ```

> **Description**
> 2 If `*guarded` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> 3 If `guarded` is a null pointer, it is treated as if it pointed to a signal set containing all signals.

> 4 All signal numbers in the signal set `guarded` shall be activated as specified in 7.14.1, except that a signal number that cannot be installed for reasons specific to the implementation is silently not installed and the call reports success. It is implementation-defined which signal numbers are not installed; an implementation should document the set of signal numbers for which it silently does not install.

> **Returns**
> 5 The `siginstall` function returns a handle that can be passed to the `siguninstall` function (7.14.2.6) to uninstall the installation, if the installation was successful; otherwise it returns a null pointer. The installation is successful if every signal number in the signal set was either installed or silently not installed as specified in paragraph 4.

> Forward references: the `siguninstall` function (7.14.2.6).
</ins>

#### Add 7.14.2.6: The `siguninstall` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> int siguninstall(void *handle);
> ```

> **Description**
> 2 All signal numbers in the signal set installed by the `siginstall` call that returned `handle` shall be deactivated as specified in 7.14.1. Uninstallation does not remove any signal deciders, and no signal decider is associated with any particular call of `siginstall` or `siguninstall`.

> 3 The handle becomes invalid after a successful call to this function. The behavior is undefined if `handle` is not the value returned by a prior call of the `siginstall` function (7.14.2.5) that has not yet been uninstalled, or if `handle` is a null pointer.

> **Returns**
> 4 The `siguninstall` function returns zero if successful and a negative value if unsuccessful.
</ins>


#### Add 7.14.2.7: The `signal_decider_create` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> void *signal_decider_create(const sigset_t *guarded, bool callfirst,
>                             sig_decide_t decider,
>                             union stdc_siginfo_value value);
> ```

> **Description**
> 2 If `*guarded` was not previously initialized by one of the signal set initialization functions (7.14.2.1), the behavior is undefined.

> 3 Installs a global signal decider function, which shall meet the requirements of 7.14.4. See 7.14.1 for how global signal decider functions are called.

> 4 If `callfirst` is true, the function is installed at the beginning of the ordered sequence of global signal deciders (7.14.1), to be called before any other global signal deciders currently installed; otherwise it is installed at the end of that ordered sequence, to be called after any other global signal deciders currently installed.

> 5 If a signal number in `guarded` is not activated at the time of the call, a decider installed for that signal is not invoked until the signal number becomes activated (7.14.1).

> 6 It is undefined behavior to call this function from within a signal decider function.

> **Returns**
> 7 The `signal_decider_create` function returns a non-null pointer that can be later passed to the `signal_decider_destroy` function (7.14.2.8) if the call is successful; otherwise it returns a null pointer.

> Forward references: the `signal_decider_destroy` function (7.14.2.8), requirements for signal handlers and signal deciders (7.14.4).
</ins>

#### Add 7.14.2.8: The `signal_decider_destroy` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> int signal_decider_destroy(void *handle);
> ```

> **Description**
> 2 The `signal_decider_destroy` function uninstalls a previously installed global signal decider function. The function is async-signal-safe only when it is called by a signal decider to destroy the storage of the decider that is currently executing; for any other call, the function is not async-signal-safe.

> 3 It is permitted to call this function from within a signal decider function only to destroy that decider's own storage. If the decider whose storage is being destroyed is currently executing, it shall complete its execution and the storage for its entry is released after that decider returns. A decider that is destroyed before it is called for the current occurrence of the signal is not called for that occurrence.

> 4 The behavior is undefined if `handle` is not the value returned by a prior call of the `signal_decider_create` function (7.14.2.7) that has not yet been destroyed, or if `handle` is a null pointer.

> **Returns**
> 5 The `signal_decider_destroy` function returns zero if successful and a negative value if unsuccessful.
</ins>

#### Add 7.14.2.9: The `stdc_raise` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> bool stdc_raise(int signo,
>                 stdc_siginfo_siginfo_t *raw_info,
>                 stdc_siginfo_context_t *raw_context);
> ```

> **Description**
> 2 The `stdc_raise` function is async-signal-safe.

> 3 This function behaves as if it called the `raise` function (7.14.2.4) with the argument `signo`, but with the additional information `raw_info` and `raw_context`.

> 4 Signal deciders are invoked for `signo` only if `signo` is activated. If `signo` is not activated, this function behaves as if it called the `raise` function and no signal deciders are invoked.

> 5 The `signo` member of the `struct stdc_siginfo` object passed to deciders is set to `signo`, and the `raw_info` and `raw_context` members are set from the arguments. If `raw_info` is a null pointer, the `raw_info` member is a null pointer, the `error_code` member is zero, and the `addr` member is a null pointer. If `raw_info` is not a null pointer, the `error_code` and `addr` members are implementation-defined.

> 6 If `signo` is not a valid signal number (7.14.1), this function returns false without raising a signal.

> **Returns**
> 7 It is unspecified whether the `stdc_raise` function ever returns. If it returns, it returns true if at least one signal decider was called, and false otherwise.
</ins>

### Add 7.14.3 Recover from signal

#### Add 7.14.3.1: The `sigguarded` function

<ins>
> **Synopsis**
> 1
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
> 2 The `sigguarded` function is async-signal-safe. The behavior is undefined if this function is called during the handling of a signal whose signal number is not activated (7.14.1). The `decider` function shall meet the requirements of 7.14.4.

> 3 Installs a thread-local signal decider function on the calling thread, saving the calling environment such that it can be restored later by a non-local jump (5.2.2.4). If a decider installed by this call returns `sig_decision_call_recovery`, a non-local jump (5.2.2.4) to the saved calling environment is performed, and the `recovery` function of this call is called, with a pointer to the `struct stdc_siginfo` object that was passed to the decider, to implement recovery from the signal raise. The thread-local signal decider installed by this call is removed when the `sigguarded` function returns or when the `recovery` function of this call begins, whichever is earlier. At that point, this `sigguarded` function call is no longer active, and any earlier `sigguarded` function call on the same thread takes over. See 7.14.1 for how thread-local signal decider functions are called.

> 4 The behavior is undefined if `signals`, `guarded`, or `decider` is a null pointer.

> 5 The behavior is undefined if `guarded` or `recovery` transfers control out of the function other than by returning, unless the `decider` abandons the guard (7.14.3.2).

> 6 If `recovery` is a null pointer, a decider returning `sig_decision_call_recovery` is treated as if it had returned `sig_decision_next_decider`.

> **Returns**
> 7 If the `guarded` function returns, the `sigguarded` function returns the value returned by `guarded`; if a signal decider initiated recovery, it returns the value returned by `recovery`. If this function is unable to install the guard, for example because of exhaustion of implementation resources, it returns `SIGGUARDED_FAILURE_VALUE`.

> Forward references: the `sigdecider_abandon` function (7.14.3.2), requirements for signal handlers and signal deciders (7.14.4).
</ins>

#### Add 7.14.3.2: The `sigdecider_abandon` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> void sigdecider_abandon(struct stdc_siginfo *rsi);
> ```

> **Description**
> 2 The `sigdecider_abandon` function is async-signal-safe.

> 3 The `sigdecider_abandon` function declares that the calling decider function will never return through the signal decider machinery (7.14.1): the `decider` function of a call to `sigguarded` (7.14.3.1), or a globally installed decider function. The argument `rsi` shall be the `stdc_siginfo` pointer passed to the calling decider function; otherwise the behavior is undefined.

> 4 It is undefined behavior to call this function from within a `recovery` function, or from code that is not currently executing within a decider function.

> 5 It is undefined behavior for the calling decider function to return after calling this function, unless a call of the `sigdecider_abandon_resume` function (7.14.3.3) with the same argument has been made in the same decider function call.

> 6 If called within a thread-local decider, that decider shall be the decider of the topmost `sigguarded` (7.14.3.1) for the calling thread, and the current guard shall be abandoned. After a guard has been abandoned, a decider returning `sig_decision_call_recovery` for that guard results in undefined behavior. The execution of the interrupted thread is not resumed.

> 7 If called within a globally installed decider, no further signal deciders are called for the current occurrence of the signal, and the interrupted thread remains suspended.

> **Returns**
> 8 The `sigdecider_abandon` function returns no value.
</ins>

#### Add 7.14.3.3: The `sigdecider_abandon_resume` function

<ins>
> **Synopsis**
> 1
> ```
> #include <signal.h>
> void sigdecider_abandon_resume(struct stdc_siginfo *rsi);
> ```

> **Description**
> 2 The `sigdecider_abandon_resume` function is async-signal-safe.

> 3 The `sigdecider_abandon_resume` function undoes a prior call of the `sigdecider_abandon` function (7.14.3.2): the argument `rsi` shall be the same `struct stdc_siginfo` pointer that was passed to a prior call of `sigdecider_abandon` made by the same decider function call on the calling thread; otherwise the behavior is undefined.

> 4 It is undefined behavior to call this function from a decider function other than the one in which the prior `sigdecider_abandon` call was made, or to undo an abandonment after the decider function from which the abandonment was called has exited.

> **Returns**
> 5 The `sigdecider_abandon_resume` function returns no value.
</ins>

### Add 7.14.4 Requirements for signal handlers and signal deciders

<ins>
> 1 The requirements of this subclause apply to a signal handler (7.14.2.3) and to a signal decider (7.14.2.7, 7.14.3.1).

> 2 When the signal occurs other than as the result of calling the `abort`, `raise`, or `stdc_raise` function, the signal handler or signal decider shall not call any function in the standard library not described as async-signal-safe (7.14.1).

> 3 A signal decider may call the `raise` (7.14.2.4), `stdc_raise` (7.14.2.9), and `abort` (7.25.5.1) functions.

> 4 The rules of 5.2.2.4 paragraph 6 apply to any object modified by a signal handler or by a signal decider. In particular, the representation of an object modified by a signal decider becomes indeterminate when the decider exits unless the object is of one of the categories specified in 5.2.2.4 paragraph 6; a signal decider can ensure that the representation of an object it modifies does not become indeterminate by calling the `sigfence` macro (7.14.1) with the object as an argument, sequenced after the last modification of the object by the decider.
</ins>


### Modifications in clause 7.17.8.1

#### In 7.17.8.1: General

> **NOTE** ... so the `atomic_flag` type is the minimum hardware-implemented type needed to conform to this document that is <del>asynchronous signal safe</del><ins>async-signal-safe (7.14.1)</ins> and that is expected to be compatible with implementation-specific extensions for shared objects between different program executions.

### Modifications in clause 7.25.5.1

#### In 7.25.5.1: The `abort` function

> The `abort` function causes abnormal program termination to occur, unless the signal `SIGABRT` is being caught and the signal handler does not return. Whether open streams with unwritten buffered data are flushed, open streams are closed, or temporary files are removed is implementation-defined. An implementation-defined form of the status *unsuccessful termination* is returned to the host environment by means of the function call `raise(SIGABRT)`.
>
> <ins>The `abort` function is async-signal-safe. If a signal decider invoked for the signal raised by this function returns `sig_decision_resume_execution`, abnormal program termination shall occur.</ins>

### Modifications in clause 7.25.5.5

#### In 7.25.5.5: The `_Exit` function

> The `_Exit` function causes normal program termination to occur and control to be returned to the host environment. No functions registered by the `atexit` function, the `at_quick_exit` function, or signal handlers registered by the `signal` function are called. The status returned to the host environment is determined in the same way as for the `exit` function (7.25.5.4). Whether open streams with unwritten buffered data are flushed, open streams are closed, or temporary files are removed is implementation-defined.
>
> <ins>The `_Exit` function is async-signal-safe.</ins>

### Modifications in clause 7.25.5.7

#### In 7.25.5.7: The `quick_exit` function

> The `quick_exit` function causes normal program termination to occur. No functions registered by the `atexit` function or signal handlers registered by the signal function are called. If a program calls the `quick_exit` function more than once, or calls the `exit` function in addition to the `quick_exit` function, the behavior is undefined. If a signal is raised while the `quick_exit` function is executing, the behavior is undefined.
>
> <ins>The `quick_exit` function is async-signal-safe.</ins>


### Modifications in clause 7.30 Threads `<threads.h>`

#### In 7.30.1 Introduction

**In paragraph 4, after the entry for `tss_t`, insert:**

<ins>
> `tss_async_signal_safe_t`
> which is a complete object type that holds an identifier for an async-signal-safe thread-specific storage pointer. The identifier is the value set by `tss_async_signal_safe_create` (7.30.6.5), and is not the thread-specific storage pointer itself;
</ins>

**After paragraph 4, insert the following new paragraphs; the existing paragraph 5 is renumbered 7:**

<ins>
> 5 The `tss_async_signal_safe_attr` structure shall contain at least the following members, in any order. The semantics of the members are expressed in the comments.
>
> ```
> // Create an instance
> int (*create)(void **dest);
>
> // Destroy an instance
> int (*destroy)(void *v);
> ```
>
> The functions pointed to by the members of this structure shall be reentrant (3.23) and shall not introduce data races.
>
> 6 EXAMPLE 1 Use `tss_async_signal_safe_get` to retrieve the thread-specific storage pointer in a signal handler
>
> ```
> #include <threads.h>
> #include <stdlib.h>
>
> struct thread_state
> {
>   int signal_count;
>   ...
> };
>
> static int create_state(void **dest)
> {
>   struct thread_state *state = malloc(sizeof(*state));
>   if (state == nullptr)
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
>   if (tss_async_signal_safe_create(&tss, &attr) == thrd_error ||
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
</ins>

#### Add 7.30.6.5: The `tss_async_signal_safe_create` function

<ins>
> **Synopsis**
> 1
> ```
> #include <threads.h>
> int tss_async_signal_safe_create(tss_async_signal_safe_t *val,
>                                  const struct tss_async_signal_safe_attr *attr);
> ```

> **Description**
> 2 Creates an async-signal-safe thread-specific storage pointer. A copy of `attr` is taken; the copy contains function pointers that are later called to create and destroy instances of the thread-specific storage. The object pointed to by `val` is set to a value that uniquely identifies the newly created instance. Each instance has its own thread-specific storage pointer for each thread, created by `tss_async_signal_safe_thread_init` (7.30.6.7).

> **Returns**
> 3 The `tss_async_signal_safe_create` function returns `thrd_success` if successful; otherwise, `thrd_error` is returned and the object pointed to by `val` is set to an indeterminate representation.
</ins>


#### Add 7.30.6.6: The `tss_async_signal_safe_destroy` function

<ins>
> **Synopsis**
> 1
> ```
> #include <threads.h>
> int tss_async_signal_safe_destroy(tss_async_signal_safe_t val);
> ```

> **Description**
> 2 Destroys a previously created async-signal-safe thread-specific storage pointer. All thread-specific storage pointers associated with this instance for which the `destroy` function pointer of the original `attr` argument has not already been called at thread exit (7.30.6.7) are destroyed using that `destroy` function pointer. The `destroy` function pointer of the original `attr` argument is called at most once for each thread-specific storage pointer, and calls of the `destroy` function pointer for different thread-specific storage pointers are not synchronized with one another.

> 3 It is undefined behavior if this function is called while any thread that has called the `tss_async_signal_safe_thread_init` function for this instance has not yet exited.

> **Returns**
> 4 The `tss_async_signal_safe_destroy` function returns `thrd_success` if successful and `thrd_error` if unsuccessful.
</ins>

#### Add 7.30.6.7: The `tss_async_signal_safe_thread_init` function

<ins>
> **Synopsis**
> 1
> ```
> #include <threads.h>
> int tss_async_signal_safe_thread_init(tss_async_signal_safe_t val);
> ```

> **Description**
> 2 Creates the thread-specific storage pointer for the calling thread for the instance identified by `val`, by invoking the `create` function pointer of the original `attr` argument. It is permitted to call this function multiple times on the same thread for the same instance; subsequent calls do not invoke the `create` function pointer again, do not change the thread-specific storage pointer, and return `thrd_success`.

> 3 At thread exit, it is implementation-defined whether the `destroy` function pointer of the original `attr` argument is called with the thread-specific storage pointer as its argument.

> 4 The behavior is undefined if this function is called for an instance that has been destroyed by the `tss_async_signal_safe_destroy` function (7.30.6.6).

> **Returns**
> 5 The `tss_async_signal_safe_thread_init` function returns `thrd_success` if successful and `thrd_error` if unsuccessful.
</ins>

#### Add 7.30.6.8: The `tss_async_signal_safe_get` function

<ins>
> **Synopsis**
> 1
> ```
> #include <threads.h>
> void *tss_async_signal_safe_get(tss_async_signal_safe_t val);
> ```

> **Description**
> 2 The `tss_async_signal_safe_get` function is async-signal-safe.

> 3 The `tss_async_signal_safe_thread_init` function (7.30.6.7) shall have been called on the same thread for the instance identified by `val`, in which case the thread-specific storage pointer created at that time for that instance is returned; otherwise the behavior is undefined.

> **Returns**
> 4 The `tss_async_signal_safe_get` function returns the thread-specific storage pointer for the calling thread for the instance identified by `val`.
</ins>

### Editorial tasks

- Delete footnote 259 (7.13.3.1): its text was moved to 5.2.2.4, where the floating-point environment and the state of open files are now enumerated in the paragraph itself.
- Delete footnote 262 (7.14.2.3 paragraph 5): its rule (a signal generated by an asynchronous signal handler is undefined behavior) is superseded — signal handlers and signal deciders may now generate signals, for example by calling the `raise`, `stdc_raise`, or `abort` functions, subject to the restrictions of 7.14.4 and paragraph 4 of 7.14.2.3 as amended.
- Footnote renumbering: after the deletions of footnotes 259 and 262, footnotes 260 and 261 are renumbered 259 and 260, and footnotes 262 onward are renumbered 261 onward.
- Paragraph numbering: the inserted paragraphs are numbered with their final numbers in this paper (5.2.2.4 paragraphs 7 through 10; 7.14.1 paragraphs 2 through 18, 20, 22, and 24; 7.30.1 paragraphs 5 and 6), and the displaced base paragraphs are renumbered accordingly: 5.2.2.4 paragraphs 7 through 17 become 11 through 21; 7.14.1 paragraphs 2 through 4 become 19, 21, and 23; 7.30.1 paragraph 5 becomes 7. The paragraphs of the new subsections 7.14.2.1, 7.14.2.2, 7.14.2.5 through 7.14.2.9, 7.14.3.1 through 7.14.3.3, 7.14.4, and 7.30.6.5 through 7.30.6.8 are numbered from 1, except that the forward references of 7.14.2.5 and 7.14.2.7 are unnumbered, as is the forward references line of the base text. The new clause 3 entry is numbered 3.23, and the entries 3.23 through 3.33 are renumbered 3.24 through 3.34.
- Annex B (summary of library functions): add entries for `sigfence`, `SIGGUARDED_FAILURE_VALUE`, `sigemptyset`, `sigfillset`, `sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`, `sigfillset_asynchronous_debug`, `sigaddset`, `sigdelset`, `sigismember`, `siginstall`, `siguninstall`, `signal_decider_create`, `signal_decider_destroy`, `stdc_raise`, `sigguarded`, `sigdecider_abandon`, `sigdecider_abandon_resume`, `tss_async_signal_safe_create`, `tss_async_signal_safe_destroy`, `tss_async_signal_safe_thread_init`, and `tss_async_signal_safe_get`.
- Annex J.1 (unspecified behavior): update item (3) for the amended 5.2.2.4 paragraph 6; add the thread on which the signal handler is called (7.14.1), the thread on which signal deciders are invoked for asynchronous category signals (7.14.1), and whether the `stdc_raise` function ever returns (7.14.2.9).
- Annex J.2 (undefined behavior): renumber the references to the signal function and the raise function in items (102) through (109) from 7.14.2.1 and 7.14.3.1 to 7.14.2.3 and 7.14.2.4; update item (105) for the amended paragraph 4 of 7.14.2.3 (a signal occurs as the result of calling the `abort`, `raise`, or `stdc_raise` function, and the signal handler calls the `raise` or `stdc_raise` function); update item (107) for the `errno` rule relocated to 7.14.1 (drop the qualifier "other than as the result of calling the `abort` or `raise` function", and change the reference from 7.14.2.1 to 7.14.1); delete item (106), whose object-access rule is no longer undefined behavior (it is now specified by the amended 5.2.2.4 paragraph 6 and by 7.14.4), and add an item for the violation of 7.14.4 (a signal handler or signal decider calls a function in the standard library not described as async-signal-safe); delete item (108), whose rule was deleted with footnote 262; and add the undefined behaviors introduced by this paper: calling the `signal` function for a signal number that is activated (7.14.2.3); use of an uninitialized `sigset_t` object (7.14.2.1, 7.14.2.2); misuse of the `siginstall`, `siguninstall`, `signal_decider_create`, `signal_decider_destroy`, `sigguarded`, `sigdecider_abandon`, and `sigdecider_abandon_resume` functions (7.14.2.5 through 7.14.2.8, 7.14.3.1 through 7.14.3.3); misuse of the `sigfence` macro (7.14.1); and misuse of the `tss_async_signal_safe_create`, `tss_async_signal_safe_destroy`, `tss_async_signal_safe_thread_init`, and `tss_async_signal_safe_get` functions (7.30.6.5 through 7.30.6.8).
- Annex J.3 (implementation-defined behavior): add the signal categories of the standard signals and of implementation-defined signals (7.14.1); which signal numbers are not installed by a call of `siginstall` (7.14.2.5); the default handling performed when a global signal decider returns `sig_decision_call_recovery` (7.14.1); the value of `SIGGUARDED_FAILURE_VALUE` (7.14.1); the additional signals in the sets produced by `sigfillset_synchronous`, `sigfillset_asynchronous_nondebug`, and `sigfillset_asynchronous_debug` (7.14.2.1); the values of the remaining members of the `struct stdc_siginfo` object, for signals other than those raised by the `stdc_raise` function (7.14.1); the values of the `error_code` and `addr` members when `raw_info` is not a null pointer (7.14.2.9); whether a later occurrence of a signal is blocked, discarded, or processed immediately while an earlier occurrence is being processed (7.14.1); and whether the `destroy` function pointer of the original `attr` argument is called at thread exit (7.30.6.7); and renumber the references to the signal function in items (9), (10), and (16) from 7.14.2.1 to 7.14.2.3.
- Annex J.5.16 (extra arguments for signal handlers): renumber the reference from 7.14.2.1 to 7.14.2.3.
- Index: add the new types, enumeration constants, functions, macros, and the term reentrant.
- No change is needed to the future library directions: 7.35.10 already reserves macro names beginning with `SIG_` and an uppercase letter (which covers `SIGGUARDED_FAILURE_VALUE`), and 7.35.20 already reserves function names, type names, and enumeration constants beginning with `tss_` and a lowercase letter (which covers the `tss_async_signal_safe_*` names).

[N2471]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2471.pdf
[N2519]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2519.pdf
[N3540]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3540.pdf
[N3765]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3765.pdf
[N3815]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3815.pdf
[N3856]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3856.htm
[N3872]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3872.pdf
[N3886]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3886.pdf
[N3924]: https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3924.htm
