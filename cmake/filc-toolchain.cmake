# Fil-C toolchain for CI and local builds.
#
# Builds this project with the Fil-C memory-safe C/C++ compiler
# (https://github.com/pizlonator/fil-c). Fil-C is a drop-in replacement for
# Clang, and the release tarballs unpack to a directory containing build/
# (the compiler) and pizfix/ (its private headers, libc and runtime). The
# compiler binaries live in the build directory of that checkout:
#   <FILC_ROOT>/bin/clang   and   <FILC_ROOT>/bin/clang++
#
# Use it by pointing CMAKE_TOOLCHAIN_FILE at this file and giving the Fil-C
# build directory, either with a cache variable or the FILC_ROOT environment
# variable:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/filc-toolchain.cmake \
#         -DFILC_ROOT=/path/to/filc-0.682-linux-x86_64/build ..
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/filc-toolchain.cmake -DFILC_ROOT=... ..
#   FILC_ROOT=/path/to/filc-0.682-linux-x86_64/build cmake \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/filc-toolchain.cmake ..
if(NOT DEFINED FILC_ROOT)
  set(FILC_ROOT "$ENV{FILC_ROOT}" CACHE PATH
    "Root directory of the Fil-C build containing bin/clang and bin/clang++")
endif()
if(NOT FILC_ROOT)
  message(FATAL_ERROR "FILC_ROOT is not set. Point it at the Fil-C build "
    "directory (containing bin/clang and bin/clang++), e.g. "
    "filc-0.682-linux-x86_64/build, or set the FILC_ROOT environment variable.")
endif()

set(CMAKE_C_COMPILER "${FILC_ROOT}/bin/clang")
set(CMAKE_CXX_COMPILER "${FILC_ROOT}/bin/clang++")
# Fil-C is its own language dialect; define __FILC__ so headers can adapt, and
# disable inline asm which Fil-C cannot compile in existing sources.
set(CMAKE_C_FLAGS_INIT "-D__FILC__=1 -DDISABLE_INLINE_ASM=1")
set(CMAKE_CXX_FLAGS_INIT "-D__FILC__=1 -DDISABLE_INLINE_ASM=1")
