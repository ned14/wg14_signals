add_code_test(async_signal_safe_tls_test SOURCES "async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(benchmark_async_signal_safe_tls_test SOURCES "benchmark_async_signal_safe_tls_test.c" FEATURES c_std_11)

add_code_test(thrd_signal_handle_test SOURCES "thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(benchmark_thrd_signal_handle_test SOURCES "benchmark_thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_sigfpe_handle_test SOURCES "thrd_sigfpe_test.c" FEATURES c_std_11)
if("-fsanitize=undefined" IN_LIST CMAKE_CXX_FLAGS OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=undefined")
  set_target_properties(thrd_signal_sigfpe_handle_test PROPERTIES
    COMPILE_FLAGS "-fno-sanitize=undefined"
  )
endif()
