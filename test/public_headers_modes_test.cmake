# Compile-only regression test for the public-header language-mode contract:
# every header in include/wg14_signals (not detail/) must compile as C89 and
# C++98 as well as the modern modes. This mirrors glibc's
# check-installed-headers modes, which caught // comments, inline, constexpr
# constructors and the SIGGUARDED_FAILURE_VALUE form in the past.
#
# Run as:
#   cmake -DSRC_DIR=<repo> -DC_COMPILER=<cc> -DCXX_COMPILER=<c++>
#         -P <this file>

if(NOT DEFINED SRC_DIR OR NOT DEFINED C_COMPILER OR NOT DEFINED CXX_COMPILER)
  message(FATAL_ERROR "SRC_DIR, C_COMPILER and CXX_COMPILER must be passed")
endif()

set(_include_dir "${SRC_DIR}/include")
set(_c_test "${CMAKE_CURRENT_LIST_DIR}/public_headers_modes_test.c")
# Strict C89/C++98 modes need a POSIX feature-test macro to expose sigset_t and
# ucontext_t on glibc/musl (the top-level build's probe runs in C11, where they
# are visible by default, so it may select none). _GNU_SOURCE is the least
# intrusive spelling that works across the supported libcs; also apply any set
# the build selected.
set(_ftm_flags -D_GNU_SOURCE)
if(DEFINED FEATURE_TEST_DEFS AND NOT FEATURE_TEST_DEFS STREQUAL "")
  foreach(_def IN LISTS FEATURE_TEST_DEFS)
    list(APPEND _ftm_flags "-D${_def}")
  endforeach()
endif()
list(REMOVE_DUPLICATES _ftm_flags)
set(_cxx_test "${CMAKE_CURRENT_LIST_DIR}/public_headers_modes_test.cpp")

foreach(_mode c89 gnu89 c11 gnu11)
  execute_process(
    COMMAND "${C_COMPILER}" "-std=${_mode}" -Werror=comment ${_ftm_flags} -fsyntax-only
            "-I${_include_dir}" "${_c_test}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "public headers do not compile in C ${_mode}:\n${_out}${_err}")
  endif()
  message(STATUS "OK: public headers compile in C ${_mode}")
endforeach()

foreach(_mode c++98 gnu++98 c++11 gnu++11)
  execute_process(
    COMMAND "${CXX_COMPILER}" "-std=${_mode}" -Werror=comment ${_ftm_flags} -fsyntax-only
            "-I${_include_dir}" "${_cxx_test}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "public headers do not compile in C++ ${_mode}:\n${_out}${_err}")
  endif()
  message(STATUS "OK: public headers compile in C++ ${_mode}")
endforeach()
