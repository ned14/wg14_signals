# Reference implementation for proposed extensions to standard C signals handling

(C) 2024 Niall Douglas [http://www.nedproductions.biz/](http://www.nedproductions.biz/)

CI: [![CI](https://github.com/ned14/wg14_signals/actions/workflows/ci.yml/badge.svg)](https://github.com/ned14/wg14_signals/actions/workflows/ci.yml)

Can be configured to be a standard library implementation for your
standard C library runtime. Licensed permissively.

## Example of use

```c
todo
```

## Supported targets

This library should work well on any POSIX implementation, as well as
Microsoft Windows. You will need a minimum of C 11 in your toolchain.
Every architecture supporting C 11 atomics should work.

Current CI test targets:

- Ubuntu Linux, x64.
- Mac OS, AArch64.
- Microsoft Windows, x64.

## Todo

https://maskray.me/blog/2021-02-14-all-about-thread-local-storage#initial-exec-tls-model-executable-preemptible
will tell you everything there is to know about how TLS is implemented

- Add option to have the signal handling TLS marked `__attribute((tls_model("initial-exec")))`.
This makes codegen async signal safe e.g. https://godbolt.org/z/a78sr4Mrc.
NOTE that this would break use of this library from within a runtime
loaded shared object! So document this loudly!

- Benchmark everything so can quote perf in WG14 paper.
