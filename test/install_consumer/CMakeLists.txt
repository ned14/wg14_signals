# Standalone consumer of the *installed* wg14_signals package. Driven by
# install_consumer/driver.cmake (via the install_consumer_test CTest target in
# test/CMakeLists.txt): it is configured against the staged install prefix, so
# find_package must locate the installed Config.cmake/ConfigVersion.cmake and
# the exported wg14_signals::wg14_signals target (analysis.md V1).

cmake_minimum_required(VERSION 3.15)
project(wg14_signals_install_consumer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The driver passes the installed package version; requiring it EXACT also
# verifies that ConfigVersion.cmake was installed and reports the right version.
set(WG14_TEST_REQ_VERSION "" CACHE STRING
    "Version to require from find_package")

if(WG14_TEST_REQ_VERSION)
  find_package(wg14_signals ${WG14_TEST_REQ_VERSION} EXACT CONFIG REQUIRED)
else()
  find_package(wg14_signals CONFIG REQUIRED)
endif()

add_executable(install_consumer_app app.c)
if(MSVC)
  target_compile_options(install_consumer_app PRIVATE /W4
                         /experimental:c11atomics)
else()
  # -Werror like the in-tree tests (plans/ideas.md 2.5): the consumer's job is
  # to prove the installed package compiles cleanly.
  target_compile_options(install_consumer_app PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_link_libraries(install_consumer_app PRIVATE
                      wg14_signals::wg14_signals)

# Copy the built app to a generator-independent location so the driver can run
# it without knowing the (possibly multi-config) output layout.
add_custom_command(
  TARGET install_consumer_app
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy
          "$<TARGET_FILE:install_consumer_app>"
          "${CMAKE_BINARY_DIR}/install_consumer_app${CMAKE_EXECUTABLE_SUFFIX}"
)
