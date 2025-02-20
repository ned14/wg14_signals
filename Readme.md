# Reference implementation for proposed extensions to standard C signals handling

(C) 2024 - 2025 Niall Douglas [http://www.nedproductions.biz/](http://www.nedproductions.biz/)

CI: [![CI](https://github.com/ned14/wg14_signals/actions/workflows/ci.yml/badge.svg)](https://github.com/ned14/wg14_signals/actions/workflows/ci.yml)

Can be configured to be a standard library implementation for your
standard C library runtime. Licensed permissively.

## Example of use

```c
/* Invoke `func` passing it `user_value`. If `SIGILL` is raised during the
execution of `func`, call `decider_func` as a filter to decide what to do.
If `decider_func` chooses to initiate recovery, perform as-if a `longjmp()`
back to before `func` was called, and invoke `recovery_func` to recover.

`thrd_signal_invoke()` can be stacked i.e. `func` can invoke subfunctions
with their own signal raise filter functions.
*/
sigset_t guarded;
sigemptyset(&guarded);
sigaddset(&guarded, SIGILL);
thrd_signal_invoke(&guarded, func, recovery_func, decider_func, user_value);
```

## Supported targets

This library should work well on any POSIX implementation, as well as
Microsoft Windows. You will need a minimum of C 11 in your toolchain.
Every architecture supporting C 11 atomics should work.

Current CI test targets:

- Ubuntu Linux, x64.
- Mac OS, AArch64.
- Microsoft Windows, x64.

Current compilers:

- GCC
- clang
- MSVC

## Configuration

You can find a number of user definable macros to override in `config.h`.
They have sensible defaults on the major platforms and toolchains.

The only one to be especially aware of is `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL`.
If your platform and toolchain has async signal safe thread local storage,
it can be used instead of a hash table which is much, much faster. Current
platform status for this support can be read all about at
https://maskray.me/blog/2021-02-14-all-about-thread-local-storage, but to
summarise:

- Most ELF platforms require the `initial-exec` TLS attribute, which we
turn on for all GNU toolchains except on Apple ones. **IF** your libc
reserves static TLS space for runtime loaded shared libraries (e.g. glibc),
you can incorporate this library into runtime loaded shared libraries
without issue. If it does not, runtime loading a shared library will fail.
In this case, either place this library in a process bootstrap shared
library or the program library, or force disable off use of async safe
thread locals.

- PE platforms (Microsoft Windows) use async thread safe thread locals
in all situations. They are initialised when their shared library is
loaded.

- Mach O platforms (Apple) do not provide async thread safe thread locals
and so the fallback hash table is always used on those platforms, which
is unfortunate.

## Todo

- Benchmark everything so can quote perf in WG14 paper.
