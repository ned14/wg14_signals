add_code_test(async_signal_safe_tls_test SOURCES "async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(benchmark_async_signal_safe_tls_test SOURCES "benchmark_async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(tss_concurrent_exit_test SOURCES "tss_concurrent_exit_test.c" FEATURES c_std_11)
set_tests_properties(tss_concurrent_exit_test PROPERTIES TIMEOUT 300)

add_code_test(benchmark_thrd_signal_handle_test SOURCES "benchmark_thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_handle_test SOURCES "thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_sigfpe_handle_test SOURCES "thrd_sigfpe_test.c" FEATURES c_std_11)
add_code_test(decider_mixed_set_test SOURCES "decider_mixed_set_test.c" FEATURES c_std_11)
set_tests_properties(decider_mixed_set_test PROPERTIES TIMEOUT 60)
add_code_test(siguninstall_raise_test SOURCES "siguninstall_raise_test.c" FEATURES c_std_11)
set_tests_properties(siguninstall_raise_test PROPERTIES TIMEOUT 300)
add_code_test(post_uninstall_reentry_test SOURCES "post_uninstall_reentry_test.c" FEATURES c_std_11)
set_tests_properties(post_uninstall_reentry_test PROPERTIES TIMEOUT 300)
add_code_test(standalone_setup_test SOURCES "standalone_setup_test.c" FEATURES c_std_11)
add_code_test(stdc_raise_zero_test SOURCES "stdc_raise_zero_test.c" FEATURES c_std_11)
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
  target_compile_options(header_only_test PRIVATE -Wall -Wextra -Wpedantic)
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
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/header_only_build_test.cmake")
set_tests_properties(header_only_build_test PROPERTIES TIMEOUT 600)

# Multi-TU C header-only consumer (analysis.md Y10): three C TUs each defining
# WG14_SIGNALS_ENABLE_HEADER_ONLY, linked without the library.
add_executable(header_only_c_multi_test
  "header_only_c_multi_test1.c" "header_only_c_multi_test2.c" "header_only_c_multi_test.c")
if(MSVC)
  target_compile_options(header_only_c_multi_test PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(header_only_c_multi_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_include_directories(header_only_c_multi_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(header_only_c_multi_test PRIVATE c_std_11)
set_target_properties(header_only_c_multi_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_test(NAME header_only_c_multi_test COMMAND $<TARGET_FILE:header_only_c_multi_test>)

# The header-only targets above do not link the library, so propagate the
# __cxa_thread_atexit() discovery (compile definition + supplying library)
# made by the top-level configure test.
set(_header_only_extra_targets header_only_test header_only_c_multi_test)
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
