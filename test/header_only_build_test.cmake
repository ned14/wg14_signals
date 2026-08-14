# Positive regression test for the header-only build (analysis.md 1.8, C3, Y10).
#
# Checks:
#   1. `-DHEADER_ONLY_BUILD=ON` configures and builds the library (1.8).
#   2. A single-TU C header-only consumer builds, links at -O0 and runs
#      without linking the library (C3).
#
# Run as: cmake -DSRC_DIR=<repo> -DBINARY_DIR=<build> -DGENERATOR=<gen>
#              [-DGENERATOR_PLATFORM=..] [-DGENERATOR_TOOLSET=..] -P <this file>
#
# Also NOT RUN under Fil-C (excluded from the Fil-C ctest run, ci.yml): the
# sub-builds below configure with the system compiler (they never receive the
# Fil-C toolchain), so on the Fil-C leg step 1 passes but the single-TU C
# consumer build/link fails because it inherits the parent configure's
# __cxa_thread_atexit supplying library, which the system linker cannot always
# satisfy. The test therefore does not exercise Fil-C on that leg and is
# excluded pending diagnosis, like the FreeBSD leg (analysis.md 5.10); the C
# header-only functionality is still covered on Fil-C by
# header_only_c_multi_test, the C++ header_only_test and the step-1
# HEADER_ONLY_BUILD=ON build.

if(NOT DEFINED SRC_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SRC_DIR and BINARY_DIR must be passed")
endif()

set(_gen)
if(DEFINED GENERATOR AND NOT GENERATOR STREQUAL "")
  list(APPEND _gen -G "${GENERATOR}")
endif()
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
  list(APPEND _gen -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
  list(APPEND _gen -T "${GENERATOR_TOOLSET}")
endif()

set(_work "${BINARY_DIR}/header_only_build_test")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

# WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS (plans/ideas.md 2.1) is forwarded to both sub-builds
# so a fallback-forced configure exercises the hash-table TLS path here too.
set(_fallback_args)
if(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS)
  list(APPEND _fallback_args -DWG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON)
endif()

# 1) -DHEADER_ONLY_BUILD=ON must build the library (analysis.md 1.8).
set(_dir "${_work}/ho_build")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SRC_DIR}" -B "${_dir}"
          -DHEADER_ONLY_BUILD=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug
          ${_gen} ${_fallback_args}
  RESULT_VARIABLE _rcfg OUTPUT_QUIET ERROR_QUIET)
if(NOT _rcfg EQUAL 0)
  message(FATAL_ERROR "HEADER_ONLY_BUILD=ON configure failed "
                      "(analysis.md 1.8 regression, configure=${_rcfg})")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_dir}" --config Debug
  RESULT_VARIABLE _rbuild OUTPUT_QUIET ERROR_QUIET)
if(NOT _rbuild EQUAL 0)
  message(FATAL_ERROR "HEADER_ONLY_BUILD=ON build failed "
                      "(analysis.md 1.8 regression, build=${_rbuild})")
endif()
message(STATUS "OK (analysis.md 1.8): HEADER_ONLY_BUILD=ON builds the library")

# 2) The single-TU C header-only consumer must build, link and run
#    (analysis.md C3).
set(_dir "${_work}/c_consumer")
set(_consumer_args)
if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  list(APPEND _consumer_args
       -DWG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT=${WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT})
  if(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB)
    list(APPEND _consumer_args
         -DWG14_SIGNALS_CXA_THREAD_ATEXIT_LIB=${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB})
  endif()
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SRC_DIR}/test/header_only_c_consumer"
          -B "${_dir}" -DCMAKE_BUILD_TYPE=Debug ${_gen} ${_consumer_args} ${_fallback_args}
  RESULT_VARIABLE _rcfg OUTPUT_QUIET ERROR_QUIET)
if(NOT _rcfg EQUAL 0)
  message(FATAL_ERROR "C header-only consumer configure failed "
                      "(analysis.md C3 regression, configure=${_rcfg})")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_dir}" --config Debug
  RESULT_VARIABLE _rbuild OUTPUT_QUIET ERROR_QUIET)
if(NOT _rbuild EQUAL 0)
  message(FATAL_ERROR "C header-only consumer build/link failed "
                      "(analysis.md C3 regression, build=${_rbuild})")
endif()
if(CMAKE_HOST_WIN32)
  set(_run "${_dir}/Debug/header_only_c_consumer")
else()
  set(_run "${_dir}/header_only_c_consumer")
endif()
execute_process(COMMAND "${_run}" RESULT_VARIABLE _rrun OUTPUT_QUIET ERROR_QUIET)
if(NOT _rrun EQUAL 0)
  message(FATAL_ERROR "C header-only consumer run failed "
                      "(analysis.md C3 regression, run=${_rrun})")
endif()
message(STATUS "OK (analysis.md C3): C header-only consumer builds, links and runs")
