# CTest driver for install_consumer_test. Verifies that `cmake --install` of
# this project produces a usable package: it stages the install into a scratch
# prefix, checks the headers / package config / version file are present,
# configures and builds the standalone consumer in test/install_consumer
# against that prefix via find_package, and runs the resulting app.
#
# Invoked with -P from test/CMakeLists.txt; all inputs arrive as -D variables:
#   INSTALL_TEST_BUILD_DIR     build dir of this project (source of --install)
#   INSTALL_TEST_CONSUMER_DIR  test/install_consumer source dir
#   INSTALL_TEST_PREFIX        scratch install prefix (removed first)
#   INSTALL_TEST_BINARY_DIR    scratch consumer build dir (removed first)
#   INSTALL_TEST_VERSION       the project version, required EXACT
#   INSTALL_TEST_CONFIG        build configuration ($<CONFIG>)
#   INSTALL_TEST_TOOLCHAIN     CMAKE_TOOLCHAIN_FILE, if the build used one
#   INSTALL_TEST_C_COMPILER     CMAKE_C_COMPILER of the parent build
#   INSTALL_TEST_C_FLAGS        CMAKE_C_FLAGS of the parent build (e.g. the
#                               /fsanitize:address flag on the MSVC CI legs)
#   INSTALL_TEST_EXE_LINKER_FLAGS  CMAKE_EXE_LINKER_FLAGS of the parent build
#   INSTALL_TEST_EXE_SUFFIX     CMAKE_EXECUTABLE_SUFFIX of the parent build
#                               (".exe" on Windows; CMAKE_EXECUTABLE_SUFFIX is
#                               not defined inside a -P script)

cmake_minimum_required(VERSION 3.15)

foreach(_req
    INSTALL_TEST_BUILD_DIR INSTALL_TEST_CONSUMER_DIR INSTALL_TEST_PREFIX
    INSTALL_TEST_BINARY_DIR INSTALL_TEST_VERSION INSTALL_TEST_CONFIG)
  if(NOT DEFINED ${_req})
    message(FATAL_ERROR "install_consumer_test: ${_req} was not passed to the driver")
  endif()
endforeach()

file(REMOVE_RECURSE "${INSTALL_TEST_PREFIX}" "${INSTALL_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${INSTALL_TEST_PREFIX}")

function(_check_res _res _step)
  if(NOT "${_res}" STREQUAL "0")
    message(FATAL_ERROR
            "install_consumer_test: ${_step} failed (exit ${_res}):\n${_out}${_err}")
  endif()
endfunction()

# 1. Install the already-built library into the scratch prefix. --config is
#    passed when known: it is required on multi-config generators to select the
#    configuration ctest is running, and is accepted (and ignored) on
#    single-config generators.
set(_install_cmd
    "${CMAKE_COMMAND}" --install "${INSTALL_TEST_BUILD_DIR}")
if(INSTALL_TEST_CONFIG)
  list(APPEND _install_cmd --config "${INSTALL_TEST_CONFIG}")
endif()
list(APPEND _install_cmd --prefix "${INSTALL_TEST_PREFIX}")
execute_process(COMMAND ${_install_cmd}
                RESULT_VARIABLE _res OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
_check_res("${_res}" "cmake --install")

# 2. The installed tree must contain the public headers and package files.
foreach(_file
    "include/wg14_signals/config.h"
    "include/wg14_signals/current_thread_id.h"
    "include/wg14_signals/thrd_signal_handle.h"
    "include/wg14_signals/tss_async_signal_safe.h"
    "include/wg14_signals/detail/impl/lock_unlock.h"
    "lib/cmake/wg14_signals/wg14_signalsConfig.cmake"
    "lib/cmake/wg14_signals/wg14_signalsConfigVersion.cmake"
    "lib/cmake/wg14_signals/wg14_signalsExports.cmake")
  if(NOT EXISTS "${INSTALL_TEST_PREFIX}/${_file}")
    message(FATAL_ERROR
            "install_consumer_test: installed file missing: ${_file}")
  endif()
endforeach()

# 3. Configure the standalone consumer against the staged install. Reuse the
#    same toolchain/compiler so cross builds (Fil-C, TSan, ASan/UBSan) still
#    link and run; the MSVC CI builds the library with /fsanitize:address, so
#    the consumer must use the same flags or linking the instrumented library
#    fails with unresolved __asan_* symbols.
set(_configure_cmd
    "${CMAKE_COMMAND}" -S "${INSTALL_TEST_CONSUMER_DIR}"
    -B "${INSTALL_TEST_BINARY_DIR}"
    -DCMAKE_PREFIX_PATH=${INSTALL_TEST_PREFIX}
    -DWG14_TEST_REQ_VERSION=${INSTALL_TEST_VERSION})
if(INSTALL_TEST_TOOLCHAIN)
  list(APPEND _configure_cmd "-DCMAKE_TOOLCHAIN_FILE=${INSTALL_TEST_TOOLCHAIN}")
else()
  list(APPEND _configure_cmd "-DCMAKE_C_COMPILER=${INSTALL_TEST_C_COMPILER}")
  if(INSTALL_TEST_C_FLAGS)
    list(APPEND _configure_cmd "-DCMAKE_C_FLAGS=${INSTALL_TEST_C_FLAGS}")
  endif()
  if(INSTALL_TEST_EXE_LINKER_FLAGS)
    list(APPEND _configure_cmd
         "-DCMAKE_EXE_LINKER_FLAGS=${INSTALL_TEST_EXE_LINKER_FLAGS}")
  endif()
endif()
execute_process(COMMAND ${_configure_cmd}
                RESULT_VARIABLE _res OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
_check_res("${_res}" "consumer configure")

set(_build_cmd "${CMAKE_COMMAND}" --build "${INSTALL_TEST_BINARY_DIR}")
if(INSTALL_TEST_CONFIG)
  list(APPEND _build_cmd --config "${INSTALL_TEST_CONFIG}")
endif()
execute_process(COMMAND ${_build_cmd}
                RESULT_VARIABLE _res OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
_check_res("${_res}" "consumer build")

# 4. Run the consumer app. A shared-library install needs the loader to find
#    the staged library: prepend the appropriate search path.
set(_app_candidates
    "${INSTALL_TEST_BINARY_DIR}/install_consumer_app${INSTALL_TEST_EXE_SUFFIX}"
    "${INSTALL_TEST_BINARY_DIR}/install_consumer_app")
set(_app)
foreach(_candidate IN LISTS _app_candidates)
  if(EXISTS "${_candidate}")
    set(_app "${_candidate}")
    break()
  endif()
endforeach()
if(NOT _app)
  message(FATAL_ERROR "install_consumer_test: built app not found in "
                      "${INSTALL_TEST_BINARY_DIR} (tried "
                      "${_app_candidates})")
endif()
if(WIN32)
  # A semicolon inside a quoted CMake argument is a list separator, so the
  # PATH value (semicolon-separated on Windows) must be assembled first and
  # every semicolon escaped, otherwise `cmake -E env` receives the path split
  # into several arguments and tries to "run" the second one.
  set(_full_path "${INSTALL_TEST_PREFIX}/bin;$ENV{PATH}")
  string(REPLACE ";" "\\;" _escaped_path "${_full_path}")
  set(_run_cmd "${CMAKE_COMMAND}" -E env "PATH=${_escaped_path}")
elseif(APPLE)
  set(_run_cmd "${CMAKE_COMMAND}" -E env
      "DYLD_LIBRARY_PATH=${INSTALL_TEST_PREFIX}/lib")
else()
  set(_run_cmd "${CMAKE_COMMAND}" -E env
      "LD_LIBRARY_PATH=${INSTALL_TEST_PREFIX}/lib")
endif()
execute_process(COMMAND ${_run_cmd} "${_app}"
                RESULT_VARIABLE _res OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
_check_res("${_res}" "consumer app run")

message(STATUS "install_consumer_test: OK")
