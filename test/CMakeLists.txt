add_code_test(async_signal_safe_tls_test SOURCES "async_signal_safe_tls_test.c" FEATURES c_std_11)
add_code_test(benchmark_async_signal_safe_tls_test SOURCES "benchmark_async_signal_safe_tls_test.c" FEATURES c_std_11)

add_code_test(benchmark_thrd_signal_handle_test SOURCES "benchmark_thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_handle_test SOURCES "thrd_signal_handle_test.c" FEATURES c_std_11)
add_code_test(thrd_signal_sigfpe_handle_test SOURCES "thrd_sigfpe_test.c" FEATURES c_std_11)
if("-fsanitize=undefined" IN_LIST CMAKE_CXX_FLAGS OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*undefined")
  set_target_properties(thrd_signal_sigfpe_handle_test PROPERTIES
    COMPILE_FLAGS "-fno-sanitize=undefined"
  )
endif()
# MSVC's address sanitiser enables the undefined behaviour sanitiser
if("/fsanitize:address" IN_LIST CMAKE_CXX_FLAGS OR CMAKE_CXX_FLAGS MATCHES "/fsanitize:address")
  set_target_properties(thrd_signal_sigfpe_handle_test PROPERTIES
    COMPILE_FLAGS "/fno-sanitize:address"
  )
endif()

add_executable(header_only_test "header_only_test1.cpp" "header_only_test2.cpp" "header_only_test.cpp")
if(MSVC)
  target_compile_options(header_only_test PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(header_only_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_include_directories(header_only_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_features(header_only_test PRIVATE c_std_11 cxx_std_11)
set_target_properties(header_only_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_test(NAME header_only_test COMMAND $<TARGET_FILE:header_only_test>)
