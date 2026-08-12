cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)
set(CMAKE_C_VISIBILITY_PRESET "hidden" CACHE STRING "Only make explicitly exported symbols visible")
set(CMAKE_CXX_VISIBILITY_PRESET "hidden" CACHE STRING "Only make explicitly exported symbols visible")
set(CMAKE_C_STANDARD "11" CACHE STRING "C standard to use")
option(HEADER_ONLY_BUILD "Build using header only (unity build)" OFF)
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
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11 cxx_std_11)
target_compile_definitions(${PROJECT_NAME} PRIVATE WG14_SIGNALS_SOURCE)
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

if(PROJECT_IS_TOP_LEVEL AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
  function(add_code_example target)
    cmake_parse_arguments(ADD_EXAMPLE "" "" "SOURCES;FEATURES;PROPERTIES" ${ARGN})
    add_executable(${target} ${ADD_EXAMPLE_SOURCES})
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
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
  endfunction()

  add_subdirectory("test")
endif()

