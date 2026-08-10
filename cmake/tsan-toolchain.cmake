# ThreadSanitizer toolchain for CI.
#
# Mirrors cmake/sanitize-toolchain.cmake (address+undefined sanitizers) but
# enables only -fsanitize=thread, on both the compiler and the linker.
# TSan is supported by GCC and Clang on Linux and by Apple Clang on macOS
# (Xcode ships the TSan runtime); MSVC has no equivalent, so this toolchain is
# used by the Linux and macOS CI jobs.
set(CMAKE_C_FLAGS_INIT "-fsanitize=thread")
set(CMAKE_CXX_FLAGS_INIT "-fsanitize=thread")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fsanitize=thread")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fsanitize=thread")
