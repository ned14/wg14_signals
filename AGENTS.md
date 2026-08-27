# Agentic coding guidelines

1. All source and header files MUST be kept compatible with the 2011 ISO
C standard, except when testing C++ header-only compilation in `test/`.
C++ source or headers must NOT appear under `include/` or `src/`.
2. Run `clang-format` on every changed header and source file. Do NOT run
`clang-format` on cmake files.
3. When building and testing, extract what to do for the current platform
from `.github/workflows/ci.yml`.
4. C++ is permitted in `test/` solely for compile-testing the public header
and verifying `extern "C"` linkage. Do NOT use C++ in any source or header
file under `include/` or `src/`.
5. Never, EVER use sleeps alone to synchronise between threads. These
cause flaky tests. ALWAYS use a proper synchronisation between threads;
sleeps within proper synchronisation are permitted.
6. When a defect or work item tracked in `plans/` is fixed, update the
document to remove that item.
7. For every public API, if its doyxgen API documentation comment contains
`THREADSAFE`, that means that function must be thread-safe. Analyse the
implementation of those APIs for every possible cause of thread-unsafety.
Be exhaustive and report your findings as severe bugs.
8. For every public API, if its doyxgen API documentation comment contains
`ASYNC-SIGNAL-SAFE`, that means that function must be safe to call from
a signal handler. Such functions must NEVER call another function which is
not also signal handler safe -- POSIX has a standardised list of signal handler
safe functions, and an `ASYNC-SIGNAL-SAFE` API must ONLY call those functions
or other functions known to be signal handler safe. Analyse the
implementation of those APIs for every possible cause of signal handler
unsafety. Be exhaustive and report your findings as severe bugs.
9. NULL inputs to public APIs causing an immediate crash SO LONG as no
data gets unexpectedly mutated or causing a potential security
vulnerability is OK - we WANT to fail fast if users supply a NULL
to an argument which is mandatory.
10. Never, EVER run `git commit` by yourself.
11. Never, EVER introduce new `_Thread_local` or `thread_local` variables
unless you can guarantee that `WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL`
will always be true. Any deviations from this that you find during analysis
is a severe defect and must be reported as such.
12. If not running on Windows, prefer to test the Windows only code using
`wine` rather than a cross compiling Mingw. If you need to examine the
source code for Windows, consider examining the source code for Reactos
(https://github.com/reactos/reactos) which is a binary compatible
reproduction of Windows.
13. NEVER use POSIX-only identifiers (e.g. `SIGUSR1`/`SIGUSR2`, `SIGCONT`,
`struct sigaction`, `sigaction()`, `SA_SIGINFO`/`SA_NODEFER`/`SA_ONSTACK`,
`siginfo_t`, `SI_USER`) in any source or test file that is compiled on
Windows. MSVC does not define these. Guard every POSIX-only definition or
block with `#ifndef _WIN32`, keep every POSIX-only identifier strictly
inside the `#else` branch of `main`, and NEVER reference a POSIX-only
identifier from the `_WIN32` branch. After adding or editing any test,
verify it compiles for a Windows target (e.g.
`clang --target=x86_64-w64-windows-gnu -fsyntax-only` against the mingw
sysroot) AND runs on the current platform. This is a recurring bug: new
tests keep introducing unguarded POSIX identifiers and failing the Windows
CI legs at compile time.
