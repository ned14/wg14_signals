cmake_minimum_required(VERSION 3.15)
project(header_only_c_consumer LANGUAGES C)

# A single-TU C header-only consumer that deliberately does NOT link the
# library (analysis.md C3 regression). The __cxa_thread_atexit() discovery
# (WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT and its supplying library) is passed in
# by test/header_only_build_test.cmake from the top-level configure test, as is
# WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS (plans/ideas.md 2.1), which forces the hash-table TLS
# path instead of the platform-native one.
add_executable(header_only_c_consumer main.c)
target_include_directories(header_only_c_consumer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_compile_features(header_only_c_consumer PRIVATE c_std_11)
if(MSVC)
  # C11 atomics (<stdatomic.h>) in the pulled-in .ipp implementations require
  # this on MSVC (mirrors test/CMakeLists.txt; plans/ideas.md 2.2).
  # /Zc:__VAOPT__ for sigfence()'s __VA_OPT__ argument counting
  # (plans/analysis.md 4.10) — MSVC only defaults it on for
  # /std:c11/c17/c++20+, not C++14/17.
  target_compile_options(header_only_c_consumer PRIVATE
    /W4 /experimental:c11atomics /Zc:__VAOPT__)
endif()

if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  target_compile_definitions(header_only_c_consumer PUBLIC
    WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
endif()
if(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB)
  target_link_libraries(header_only_c_consumer PRIVATE
    ${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB})
endif()
if(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS)
  target_compile_definitions(header_only_c_consumer PUBLIC
    WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0)
endif()
