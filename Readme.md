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

