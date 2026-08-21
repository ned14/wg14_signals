cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
set(CMAKE_C_VISIBILITY_PRESET "hidden" CACHE STRING "Only make explicitly exported symbols visible")
set(CMAKE_CXX_VISIBILITY_PRESET "hidden" CACHE STRING "Only make explicitly exported symbols visible")
set(CMAKE_C_STANDARD "11" CACHE STRING "C standard to use")
option(HEADER_ONLY_BUILD "Build using header only (unity build)" OFF)
# Force the tss_async_signal_safe hash-table TLS path on every platform instead
# of the platform-native async-signal-safe TLS (plans/ideas.md 2.1). config.h
# auto-selects the native path on Linux, so without this option the fallback is
# only ever exercised on macOS; this option lets CI run the fallback under the
# strongest sanitizers everywhere. PUBLIC so consumers (and the installed
# package's exported target) select the same path.
option(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS
  "Force the tss_async_signal_safe hash-table TLS path on all platforms" OFF)
# Prove the embedder override interface (plans/llvm-project-fork.md Phase 1.2):
# compile the library with the three override hooks
# (WG14_SIGNALS_SIGACTION/ABORT/KILL_SELF in config.h) redirected to distinct
# wrapper functions in test/override_probe.{h,c}. An embedding standard C
# library redefines exactly these hooks to its own kernel-facing layer; the
# probe substitutes pass-through wrappers to the host libc and the full test
# suite must pass unchanged. POSIX platforms only (the wrappers need
# sigaction/pthread_kill; the Windows backend is not built in libc).
option(WG14_SIGNALS_OVERRIDE_PROBE
  "Build the library with the embedder override hooks redirected to wrapper functions" OFF)
set(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB "" CACHE STRING
  "Library that supplies __cxa_thread_atexit() (empty if the C runtime does)")

project(wg14_signals VERSION 1.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
enable_testing()
include(CheckCSourceCompiles)
check_c_source_compiles("#include <setjmp.h>\nint main(void) { jmp_buf buf; return _setjmp(buf); }" LIBC_HAS__SETJMP)

# Discover __cxa_thread_atexit() (the Itanium ABI thread-local destructor
# registration primitive) and which library supplies it, so thread_atexit() can
# use it preferentially (see detail/impl/thread_atexit.c.ipp). The probe is a
# compile+link test: its return value is deliberately ignored by thread_atexit()
# because it is not reliable on every platform that supplies it (e.g. macOS
# returns garbage), so only symbol availability is probed.
#
# On Linux the C library (glibc) exports only the low-level
# __cxa_thread_atexit_impl (since GLIBC_2.18); the caller-facing
# __cxa_thread_atexit() wrapper is supplied by the C++ runtime libraries.
# libsupc++ is preferred: it is a static archive that provides the wrapper
# without dragging in the libstdc++/libc++ standard-library runtime, so a C
# consumer links it with no C++ runtime dependency. libc++abi is the clang
# equivalent (dynamic); libstdc++ also works but pulls the full C++ runtime.
# macOS and FreeBSD C runtimes that supply the wrapper directly are picked up
# by the no-extra-library probe below. On Windows the MSVC CRT provides no
# __cxa_thread_atexit() at all (thread_atexit() uses the IMAGE_TLS_DIRECTORY
# mechanism there), and the candidate libraries are ELF-toolchain libraries,
# so they are only probed on non-Windows.
set(_cxa_probe_src "
extern int __cxa_thread_atexit(void (*func)(void *), void *obj, void *dso_symbol);
static void run(void *o) { (void) o; }
static void dso_symbol(void) {}
int main(void) { return __cxa_thread_atexit(run, (void *) 0, (void *) &dso_symbol); }
")
set(CMAKE_REQUIRED_LIBRARIES "")
check_c_source_compiles("${_cxa_probe_src}" WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
if(NOT WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT AND NOT WIN32)
  foreach(_candidate supc++ c++abi stdc++)
    set(CMAKE_REQUIRED_LIBRARIES "${_candidate}")
    unset(_wg14_signals_cxa_result CACHE)
    check_c_source_compiles("${_cxa_probe_src}" _wg14_signals_cxa_result)
    if(_wg14_signals_cxa_result)
      set(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT TRUE)
      set(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB "${_candidate}")
      break()
    endif()
  endforeach()
endif()
set(CMAKE_REQUIRED_LIBRARIES "")
if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  if(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB)
    message(STATUS "__cxa_thread_atexit() found, supplied by ${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB}")
  else()
    message(STATUS "__cxa_thread_atexit() found, supplied by the C runtime")
  endif()
else()
  message(STATUS "__cxa_thread_atexit() not found; thread_atexit() uses the C++/pthread-key/MSVC-TLS-directory fallback")
endif()

# thread_atexit(): the C implementation (using __cxa_thread_atexit()) is used
# when __cxa_thread_atexit() is available; otherwise the C++ implementation is
# preferred and the C source is not compiled. Only one is ever compiled or
# linked.
if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  set(THREAD_ATEXIT_SOURCE "src/wg14_signals/thread_atexit.c")
else()
  set(THREAD_ATEXIT_SOURCE "src/wg14_signals/thread_atexit.cpp")
endif()
set(LIBRARY_SOURCES
  "src/wg14_signals/current_thread_id.c"
  "src/wg14_signals/tss_async_signal_safe.c"
  $<$<NOT:$<PLATFORM_ID:Windows>>:src/wg14_signals/thrd_signal_handle_posix.c>
  $<$<PLATFORM_ID:Windows>:src/wg14_signals/thrd_signal_handle_windows.c>
  "${THREAD_ATEXIT_SOURCE}"
)

add_library(${PROJECT_NAME} ${LIBRARY_SOURCES})
if(HEADER_ONLY_BUILD)
  target_compile_definitions(${PROJECT_NAME} PUBLIC WG14_SIGNALS_ENABLE_HEADER_ONLY)
endif()
if(WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS)
  target_compile_definitions(${PROJECT_NAME}
                             PUBLIC WG14_SIGNALS_HAVE_ASYNC_SAFE_THREAD_LOCAL=0)
  message(STATUS "WG14_SIGNALS_ALWAYS_USE_FALLBACK_TLS=ON: forcing the tss_async_signal_safe hash-table TLS path")
endif()
if(WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  target_compile_definitions(${PROJECT_NAME} PUBLIC WG14_SIGNALS_HAVE__CXA_THREAD_ATEXIT)
  if(WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB)
    target_link_libraries(${PROJECT_NAME} PUBLIC ${WG14_SIGNALS_CXA_THREAD_ATEXIT_LIB})
  endif()
endif()
# FreeBSD keeps its C11 <threads.h> implementation (thrd_*, mtx_*, ...) in a
# separate base library, libstdthreads, rather than in libc or libpthread.
# Link it PUBLIC so consumers of the library (and its header-only inline
# implementations) get it transitively; only targets that deliberately do not
# link the library (test/header_only_test) need their own link.
target_link_libraries(${PROJECT_NAME} PUBLIC $<$<PLATFORM_ID:FreeBSD>:stdthreads>)
# PUBLIC so the C11/C++11 floor propagates to find_package consumers: the public
# headers use C11 constructs (stdatomic.h, _Static_assert, compound literals),
# so a consumer compiling in C99 mode must be told to raise its standard or the
# include fails (analysis.md CSTD).
target_compile_features(${PROJECT_NAME} PUBLIC c_std_11 cxx_std_11)
target_compile_definitions(${PROJECT_NAME} PRIVATE WG14_SIGNALS_SOURCE)
if(WG14_SIGNALS_OVERRIDE_PROBE)
  if(WIN32)
    message(WARNING "WG14_SIGNALS_OVERRIDE_PROBE is only supported on POSIX platforms; ignoring")
  else()
    target_sources(${PROJECT_NAME} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/test/override_probe.c")
    if(MSVC)
      target_compile_options(${PROJECT_NAME} PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/test/override_probe.h")
    else()
      target_compile_options(${PROJECT_NAME} PRIVATE
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/test/override_probe.h")
    endif()
    message(STATUS "WG14_SIGNALS_OVERRIDE_PROBE=ON: routing sigaction/abort/pthread_kill(pthread_self(), ...) through the embedder override hooks")
  endif()
endif()
# Feature-test-macro discipline (plans/ideas.md 2.2): the library needs the
# POSIX signal surface — NSIG (used unguarded by the signo-to-sighandler map,
# finding `NSIG`), the sigset functions, and SIGSYS/SIGXCPU/SIGXFSZ
# (`SIGN`). Whether the toolchain exposes those by default is a
# property of the compiler mode and the libc, not of the OS name: most libcs
# expose them in the default (GNU-extension) compile mode, while strict-POSIX
# modes and some libcs (e.g. Apple/BSD hide NSIG under
# _POSIX_C_SOURCE/_XOPEN_SOURCE) hide them until a feature-test macro is
# defined. Rather than enumerate platforms, probe the actual toolchain from the
# *least* intrusive configuration upward: first with no extra defines at all,
# then adding defines one level at a time until the probe compiles. PRIVATE:
# consumers compile their own context (see the mirrors in
# test/install_consumer/app.c and the white-box tests).
if(NOT MSVC)
  set(_ftm_probe_src
    "#include <pthread.h>\n#include <signal.h>\n#include <string.h>\n#ifndef NSIG\n#error NSIG missing\n#endif\nint main(void)\n{\n  sigset_t set;\n  memset(&set, 0, sizeof(set));\n  sigemptyset(&set);\n  sigfillset(&set);\n  sigaddset(&set, SIGSYS);\n  sigaddset(&set, SIGXCPU);\n  sigaddset(&set, SIGXFSZ);\n  return (NSIG > 0 && sigismember(&set, SIGSEGV) == 0) ? 1 : 0;\n}\n"
  )
  # Escalation ladder, least intrusive first. _GNU_SOURCE alone is the usual
  # winner (it subsumes the whole trio on glibc/musl and is harmless on Apple/
  # BSD); _POSIX_C_SOURCE/_XOPEN_SOURCE cover libcs that ignore _GNU_SOURCE.
  set(_ftm_defs_0)
  set(_ftm_defs_1 _GNU_SOURCE)
  set(_ftm_defs_2 _POSIX_C_SOURCE=200809L)
  set(_ftm_defs_3 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
  set(_ftm_defs_4 _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _GNU_SOURCE)
  set(_ftm_found FALSE)
  foreach(_ftm_level RANGE 0 4)
    set(CMAKE_REQUIRED_DEFINITIONS)
    foreach(_ftm_d IN LISTS _ftm_defs_${_ftm_level})
      list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${_ftm_d}")
    endforeach()
    check_c_source_compiles("${_ftm_probe_src}"
      WG14_SIGNALS_FT_PROBE_${_ftm_level})
    if(WG14_SIGNALS_FT_PROBE_${_ftm_level})
      set(_ftm_choice ${_ftm_defs_${_ftm_level}})
      set(_ftm_found TRUE)
      break()
    endif()
  endforeach()
  unset(CMAKE_REQUIRED_DEFINITIONS)
  if(_ftm_found)
    if(_ftm_choice)
      target_compile_definitions(${PROJECT_NAME} PRIVATE ${_ftm_choice})
    endif()
    message(STATUS "Feature-test macros: ${_ftm_choice} (least intrusive set whose probe compiled)")
  else()
    message(WARNING "Feature-test macros: no tested define set exposes NSIG and the signal set on this toolchain; compiling with none")
  endif()
endif()
# Windows SDK floor (plans/ideas.md 2.3): the Windows backend uses
# AddVectoredContinueHandler / SetUnhandledExceptionFilter (Vista-and-later SDK
# declarations) and thread_atexit() uses the IMAGE_TLS_DIRECTORY mechanism.
# Enforce the floor consistently — PUBLIC so consumers and header-only builds of
# the .ipp implementations get the same declarations; a too-low _WIN32_WINNT
# explicitly set by a consumer is rejected by the guard in
# thrd_signal_handle_windows.c.ipp.
if(WIN32)
  target_compile_definitions(${PROJECT_NAME}
    PUBLIC _WIN32_WINNT=0x0600 WINVER=0x0600)
endif()
if(LIBC_HAS__SETJMP)
  target_compile_definitions(${PROJECT_NAME} PRIVATE WG14_SIGNALS_HAVE__SETJMP)
endif()
target_include_directories(${PROJECT_NAME} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
set_target_properties(${PROJECT_NAME} PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /experimental:c11atomics)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}Exports
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# The public headers are the library's interface; without this nothing under
# <wg14_signals/...> is available to a find_package consumer (analysis.md V1).
install(DIRECTORY include/wg14_signals
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# configure_package_config_file (not plain configure_file) defines the
# check_required_components() helper the config template calls and keeps the
# config relocatable; write_basic_package_version_file makes versioned
# find_package(wg14_signals N.N) requests work (analysis.md V1).
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/ProjectConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)
install(EXPORT ${PROJECT_NAME}Exports
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND
   (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
  function(add_code_example target)
    cmake_parse_arguments(ADD_EXAMPLE "" "" "SOURCES;FEATURES;PROPERTIES" ${ARGN})
    add_executable(${target} ${ADD_EXAMPLE_SOURCES})
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
    else()
      # Tests build with -Werror like the library (plans/ideas.md 2.5,
      # (`WXMS`), so test-only warnings surface instead of being
      # invisible.
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    target_compile_features(${target} PRIVATE ${ADD_EXAMPLE_FEATURES})
    target_link_libraries(${target} PRIVATE ${PROJECT_NAME})
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
      ${ADD_EXAMPLE_PROPERTIES}
    )
  endfunction()
  function(add_code_test target)
    add_code_example(${target} ${ARGN})
    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    # A hang is then reported against the specific test instead of only after
    # the CI-wide `ctest --timeout 300` expires (plans/ideas.md 1.3). Tests that
    # need longer, or that bound a pathological hang, override TIMEOUT with
    # their own set_tests_properties() in test/CMakeLists.txt.
    set_tests_properties(${target} PROPERTIES TIMEOUT 60)
  endfunction()

  add_subdirectory("test")
endif()

