add_code_test(async_signal_safe_tls_test SOURCES "async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(benchmark_async_signal_safe_tls_test SOURCES "benchmark_async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(tss_concurrent_exit_test SOURCES "tss_concurrent_exit_test.c" FEATURES c_std_11)
set_tests_properties(tss_concurrent_exit_test PROPERTIES TIMEOUT 300)
# Regression test for analysis.md UCLK: tss_async_signal_safe_destroy must run
# the user's attr.destroy callbacks with mem->lock released (a re-entrant get()
# from the callback would pre-fix deadlock on the spinlock). Bound the timeout
# so the pre-fix hang is a fast failure instead of a ctest hang.
add_code_test(tss_destroy_reentrancy_test SOURCES "tss_destroy_reentrancy_test.c" FEATURES c_std_11)
set_tests_properties(tss_destroy_reentrancy_test PROPERTIES TIMEOUT 60)

add_code_test(benchmark_thrd_signal_handle_test SOURCES "benchmark_thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_handle_test SOURCES "thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_sigfpe_handle_test SOURCES "thrd_sigfpe_test.c" FEATURES c_std_11)
add_code_test(decider_mixed_set_test SOURCES "decider_mixed_set_test.c" FEATURES c_std_11)
set_tests_properties(decider_mixed_set_test PROPERTIES TIMEOUT 60)
add_code_test(siguninstall_raise_test SOURCES "siguninstall_raise_test.c" FEATURES c_std_11)
set_tests_properties(siguninstall_raise_test PROPERTIES TIMEOUT 300)
# White-box regression test for analysis.md 3.8: a partial siginstall() must
# roll back the signals already installed in that call. It links with
# --wrap=calloc (GNU/BSD ld) to fail the per-signal sighandler_info allocation
# for a chosen later signal; the header-only build compiles the .ipp into the
# test TU so the interposer sees the .ipp's calloc calls. Excluded on MSVC and
# Apple (their linkers do not support --wrap), and under Fil-C: Fil-C
# "pizlonates" every symbol (e.g. __real_calloc becomes
# pizlonated___real_calloc), so the --wrap rewrites never match the mangled
# names, and libpizlo.so's own wrapped calloc reference cannot be bound either
# ("undefined reference to `__wrap_calloc'"). The rollback behaviour remains
# covered by the GCC/Clang Linux and FreeBSD legs.
if(NOT MSVC AND NOT APPLE AND NOT CMAKE_C_COMPILER_ID MATCHES "MSVC"
   AND NOT CMAKE_C_COMPILER MATCHES "[Ff][Ii][Ll]-?[Cc]")
  add_code_example(siginstall_rollback_test SOURCES "siginstall_rollback_test.c" FEATURES c_std_11)
  target_link_options(siginstall_rollback_test PRIVATE "LINKER:--wrap=calloc")
  add_test(NAME siginstall_rollback_test COMMAND $<TARGET_FILE:siginstall_rollback_test>)
  set_tests_properties(siginstall_rollback_test PROPERTIES TIMEOUT 60)
endif()
add_code_test(post_uninstall_reentry_test SOURCES "post_uninstall_reentry_test.c" FEATURES c_std_11)
set_tests_properties(post_uninstall_reentry_test PROPERTIES TIMEOUT 300)
add_code_test(standalone_setup_test SOURCES "standalone_setup_test.c" FEATURES c_std_11)
add_code_test(stdc_raise_zero_test SOURCES "stdc_raise_zero_test.c" FEATURES c_std_11)
# Regression test for analysis.md VSDT: the N3924 7.14.2.1/7.14.2.2 sigset
# helpers return int ("always returns zero"), so the documented
# `if(sigemptyset(&ss) != 0)` idiom must compile on Windows too (pre-fix the
# Windows helpers returned void and this failed to compile there).
add_code_test(sigset_helpers_contract_test SOURCES "sigset_helpers_contract_test.c" FEATURES c_std_11)
add_code_test(stdc_raise_null_info_test SOURCES "stdc_raise_null_info_test.c" FEATURES c_std_11)
# Regression test for analysis.md NSIH: stdc_raise(signo, NULL, NULL) handed off
# to a pre-existing SA_SIGINFO handler with a NULL siginfo_t *; the POSIX
# invoke_sigaction must synthesise a minimal siginfo_t (si_signo/si_code set)
# instead. POSIX-only, no-op on Windows.
add_code_test(nsih_siginfo_handoff_test SOURCES "nsih_siginfo_handoff_test.c" FEATURES c_std_11)
# Regression test for analysis.md DFLT: the SIG_DFL default-action re-raise in
# the POSIX invoke_sigaction must restore the library's installed handler, not
# leave the kernel handler reset to SIG_DFL (stop/continue default signals).
add_code_test(sig_default_preserve_test SOURCES "sig_default_preserve_test.c" FEATURES c_std_11)
# Regression test for analysis.md NEGS: out-of-range/negative signo must be
# rejected before the POSIX frame walk, so stdc_raise(-1) / stdc_raise(NSIG)
# return false without invoking any frame decider (on macOS/BSD sigismember's
# shift-count wrap pre-fix aliased another signal's frame decider).
add_code_test(out_of_range_signo_test SOURCES "out_of_range_signo_test.c" FEATURES c_std_11)
# White-box regression test for analysis.md UNKN: raw_signal_handler's fallback
# for a signal with no map entry must not permanently reset the kernel handler to
# SIG_DFL (the installed handler must survive the default-action pass-on). Uses
# header-only mode so the internal map/state statics are reachable to manufacture
# the transient "unknown signal" window.
add_code_test(sig_unknown_handler_test SOURCES "sig_unknown_handler_test.c" FEATURES c_std_11)
add_code_test(stdc_raise_uninstalled_test SOURCES "stdc_raise_uninstalled_test.c" FEATURES c_std_11)
add_code_test(sigguarded_tss_init_test SOURCES "sigguarded_tss_init_test.c" FEATURES c_std_11)
# Regression test for analysis.md SIGM: the Windows sigguarded() frame filter
# must not treat sigismember()'s -1 (signo outside 1..32) as membership, so a
# non-signal exception (C++ /EHa code 0xE06D7363 -> signo 0, or a user-range
# raise -> bogus signo) skips the frame decider and propagates past the guard.
# MSVC-only: sigguarded()'s __try/__except and the test's outer SEH handler
# need MSVC; MinGW has no SEH alternative (deliberate #error in the library).
add_code_test(sigguarded_nonsignal_exception_test SOURCES "sigguarded_nonsignal_exception_test.c" FEATURES c_std_11)
set_tests_properties(sigguarded_nonsignal_exception_test PROPERTIES TIMEOUT 60)
add_code_test(guard_abandon_test SOURCES "guard_abandon_test.c" FEATURES c_std_11)
set_tests_properties(guard_abandon_test PROPERTIES TIMEOUT 60)
add_code_test(nested_decider_rsi_test SOURCES "nested_decider_rsi_test.c" FEATURES c_std_11)
set_tests_properties(nested_decider_rsi_test PROPERTIES TIMEOUT 60)
add_code_test(install_sighandler_lock_test SOURCES "install_sighandler_lock_test.c" FEATURES c_std_11)
# Before analysis.md 2.20/Y1 was fixed, a failed install leaked the global
# spinlock and this white-box test spun forever; bound it so the regression is
# a fast failure instead of a hang.
set_tests_properties(install_sighandler_lock_test PROPERTIES TIMEOUT 60)
# White-box regression test for analysis.md 2.21/Z1: forces the verstable
# signo_to_sighandler_map_t branch (NSIG >= 1024) which no CI libc reaches.
add_code_test(signo_map_verstable_init_test SOURCES "signo_map_verstable_init_test.c" FEATURES c_std_11)
# Regression test for analysis.md 2.23/AA1: signal_decider_destroy after a
# siguninstall -> siginstall cycle must not touch the orphaned decider node.
add_code_test(decider_orphan_reinstall_test SOURCES "decider_orphan_reinstall_test.c" FEATURES c_std_11)
# Regression test for analysis.md SDCF: signal_decider_destroy must return 0
# (N3924 7.14.2.8) whenever the handle is removed, even when every slot is NULL
# (no handler installed at create time); only a NULL handle is a failure.
# Regression test for the reentrant signal_decider_destroy contract (plans
# proposed-wording-fixes.md 7.3/17, N3924 7.14.2.8): destroying a decider from
# within a decider function -- its own node (deferred free via the refcount
# protocol) or a sibling node (mid-walk LIST_REMOVE relinking) -- must not
# deadlock, and the destroyed decider must not be invoked again.
add_code_test(decider_destroy_return_test SOURCES "decider_destroy_return_test.c" FEATURES c_std_11)
add_code_test(decider_reentrant_destroy_test SOURCES "decider_reentrant_destroy_test.c" FEATURES c_std_11)
add_code_test(recovery_null_loop_test SOURCES "recovery_null_loop_test.c" FEATURES c_std_11)
set_tests_properties(recovery_null_loop_test PROPERTIES TIMEOUT 60)

add_code_test(sigfence_fence_test SOURCES "sigfence_fence_test.c" FEATURES c_std_11)

# Codegen regression test for sigfence() (plans/analysis.md 2.9, W11): the
# fence must force the fenced locals through memory at Release optimization.
# Compiles two probes with the real toolchain and inspects the assembly, so
# this needs a C compiler but no runner.
add_test(NAME sigfence_codegen_test
         COMMAND ${CMAKE_COMMAND}
                 -DSRC_DIR=${PROJECT_SOURCE_DIR}
                 -DBINARY_DIR=${PROJECT_BINARY_DIR}
                 -DCOMPILER_ID=${CMAKE_C_COMPILER_ID}
                 -DGENERATOR=${CMAKE_GENERATOR}
                 -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
                 -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/sigfence_codegen_test.cmake")
set_tests_properties(sigfence_codegen_test PROPERTIES TIMEOUT 300)

add_executable(header_only_test "header_only_test1.cpp" "header_only_test2.cpp" "header_only_test.cpp")
if(MSVC)
  target_compile_options(header_only_test PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(header_only_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_include_directories(header_only_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(header_only_test PRIVATE c_std_11 cxx_std_11)
# This target creates threads (thrd_create/thrd_join in header_only_test.cpp),
# so on FreeBSD it must link libstdthreads itself: it deliberately does NOT
# link the library, so it does not inherit the library's PUBLIC stdthreads link.
target_link_libraries(header_only_test PRIVATE $<$<PLATFORM_ID:FreeBSD>:stdthreads>)
set_target_properties(header_only_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_test(NAME header_only_test COMMAND $<TARGET_FILE:header_only_test>)

# Install + find_package consumer test: stages `cmake --install` into a scratch
# prefix, checks the headers / package config / version file landed, then
# configures, builds and runs the standalone consumer in install_consumer/
# against that prefix. This is what catches install/export regressions
# (analysis.md V1) which a build-and-ctest-only CI would otherwise never see.
add_test(
  NAME install_consumer_test
  COMMAND ${CMAKE_COMMAND}
    -DINSTALL_TEST_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DINSTALL_TEST_CONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install_consumer
    -DINSTALL_TEST_PREFIX=${CMAKE_BINARY_DIR}/install_test_prefix
    -DINSTALL_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/install_test_consumer
    -DINSTALL_TEST_VERSION=${PROJECT_VERSION}
    -DINSTALL_TEST_CONFIG=$<CONFIG>
    -DINSTALL_TEST_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}
    -DINSTALL_TEST_C_COMPILER=${CMAKE_C_COMPILER}
    -DINSTALL_TEST_C_FLAGS=${CMAKE_C_FLAGS}
    -DINSTALL_TEST_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
    -DINSTALL_TEST_EXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/install_consumer/driver.cmake
)
set_tests_properties(install_consumer_test PROPERTIES TIMEOUT 180)

# Positive regression test for the header-only build (analysis.md 1.8, C3):
# -DHEADER_ONLY_BUILD=ON must build, and the single-TU C header-only consumer
# must build, link and run.
add_test(NAME header_only_build_test
         COMMAND ${CMAKE_COMMAND}
                 -DSRC_DIR=${PROJECT_SOURCE_DIR}
                 -DBINARY_DIR=${PROJECT_BINARY_DIR}
                 -DGENERATOR=${CMAKE_GENERATOR}
                 -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
                 -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
                 -DWG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT=${WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT}
                 -DWG14_SIGNALS_CXA_THREAD_ATEXIT_LIB=${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB}
                 -DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=${WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS}
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/header_only_build_test.cmake")
set_tests_properties(header_only_build_test PROPERTIES TIMEOUT 600)

# Multi-TU C header-only consumer (analysis.md Y10): three C TUs each defining
# WG14_SIGNALS_ENABLE_HEADER_ONLY, linked without the library.
add_executable(header_only_c_multi_test
  "header_only_c_multi_test1.c" "header_only_c_multi_test2.c" "header_only_c_multi_test.c")
if(MSVC)
  target_compile_options(header_only_c_multi_test PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(header_only_c_multi_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_include_directories(header_only_c_multi_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(header_only_c_multi_test PRIVATE c_std_11)
set_target_properties(header_only_c_multi_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_test(NAME header_only_c_multi_test COMMAND $<TARGET_FILE:header_only_c_multi_test>)

# White-box regression test for analysis.md TIDR (thread-ID reuse with stale
# hash-table entries): header-only mode compiles tss_async_signal_safe.c.ipp
# into the TU so the test can manufacture a stale map entry under an old
# generation and prove a later incarnation of the tid does not observe it. It
# deliberately does NOT link the library -- it references the internal
# generation cache and composite-key helper, which are not exported symbols.
add_executable(tid_reuse_test "tid_reuse_test.c")
if(MSVC)
  target_compile_options(tid_reuse_test PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(tid_reuse_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_include_directories(tid_reuse_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(tid_reuse_test PRIVATE c_std_11)
# This target creates threads (thrd_create/thrd_join in tid_reuse_test.c), so
# on FreeBSD it must link libstdthreads itself: it deliberately does NOT link
# the library, so it does not inherit the library's PUBLIC stdthreads link.
target_link_libraries(tid_reuse_test PRIVATE $<$<PLATFORM_ID:FreeBSD>:stdthreads>)
set_target_properties(tid_reuse_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_test(NAME tid_reuse_test COMMAND $<TARGET_FILE:tid_reuse_test>)
set_tests_properties(tid_reuse_test PROPERTIES TIMEOUT 60)

# The header-only targets above do not link the library, so propagate the
# __cxa_thread_atexit() discovery (compile definition + supplying library)
# made by the top-level configure test, and the WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS choice
# (plans/ideas.md 2.1) so a fallback-forced build exercises the hash-table TLS
# path in the header-only consumers too.
set(_header_only_extra_targets header_only_test header_only_c_multi_test tid_reuse_test)
if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  foreach(_t IN LISTS _header_only_extra_targets)
    target_compile_definitions(${_t} PUBLIC WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  endforeach()
endif()
if(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB)
  foreach(_t IN LISTS _header_only_extra_targets)
    target_link_libraries(${_t} PRIVATE ${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB})
  endforeach()
endif()
if(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS)
  foreach(_t IN LISTS _header_only_extra_targets)
    target_compile_definitions(${_t} PUBLIC
      WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0)
  endforeach()
endif()
