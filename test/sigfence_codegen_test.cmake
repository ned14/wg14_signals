# Codegen regression test for sigfence() (plans/analysis.md 2.9, W11).
#
# sigfence() promises a compiler-only memory barrier that forces the fenced
# local variables through memory (their current values committed to memory at
# the fence, and reloaded afterwards), so signal handlers and other threads
# observe consistent values. We compile two near-identical probes at Release
# optimization (-O3 on GNU/clang, /O2 on MSVC) and inspect the emitted
# assembly:
#
#   - the fenced probe MUST spill its local to the stack and reload it around
#     the fence (stack-memory traffic);
#   - the unfenced reference probe MUST be register-only (no stack traffic),
#     proving the discriminator is the fence itself.
#
# If a future change lets any optimizer (same-TU inlining, LTO/LTCG, ...)
# elide the fence, the fenced probe becomes register-only like the reference
# and this test fails.
#
# Run as: cmake -DSRC_DIR=<repo> -DBINARY_DIR=<build> -DCOMPILER_ID=<id>
#              [-DGENERATOR=..] [-DGENERATOR_PLATFORM=..] [-DGENERATOR_TOOLSET=..]
#              -P <this file>

if(NOT DEFINED SRC_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED COMPILER_ID)
  message(FATAL_ERROR "SRC_DIR, BINARY_DIR and COMPILER_ID must be passed")
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

set(_work "${BINARY_DIR}/sigfence_codegen_test")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

# The probe is a header-only consumer so this also covers the header-only path.
file(WRITE "${_work}/fenced.c"
"#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1\n"
"#include \"wg14_signals/thrd_signal_handle.h\"\n"
"int sigfence_probe(int input)\n"
"{\n"
"  int a = input * 3 + 1;\n"
"  sigfence(a);\n"
"  return a + 1;\n"
"}\n")
file(WRITE "${_work}/unfenced.c"
"#define WG14_SIGNALS_ENABLE_HEADER_ONLY 1\n"
"#include \"wg14_signals/thrd_signal_handle.h\"\n"
"int sigfence_probe_unfenced(int input)\n"
"{\n"
"  int a = input * 3 + 1;\n"
"  return a + 1;\n"
"}\n")

if(COMPILER_ID STREQUAL "MSVC")
  set(_asm_ext "asm")
  set(_asm_flags "/FAsc /c /O2 /experimental:c11atomics")
else()
  set(_asm_ext "s")
  set(_asm_flags "-std=c11 -O3 -S")
endif()

# A throwaway project whose only job is to compile the two probes to assembly
# via the real toolchain (so the MSVC environment is set up correctly on
# Windows). The listing lands in the build directory because the custom
# commands run with WORKING_DIRECTORY set. No DEPENDS on the probe files: the
# outer script recreates this whole directory on every run, so the outputs
# never persist and the commands always run.
file(WRITE "${_work}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.15)\n"
"project(sigfence_codegen_probe LANGUAGES C)\n"
"\n"
"add_custom_command(\n"
"  OUTPUT fenced.${_asm_ext}\n"
"  COMMAND \"\${CMAKE_C_COMPILER}\" ${_asm_flags}\n"
"          -I \"\${WG14_SIGFENCE_INCLUDE_DIR}\"\n"
"          \"\${CMAKE_CURRENT_SOURCE_DIR}/fenced.c\"\n"
"  WORKING_DIRECTORY \"\${CMAKE_CURRENT_BINARY_DIR}\")\n"
"add_custom_command(\n"
"  OUTPUT unfenced.${_asm_ext}\n"
"  COMMAND \"\${CMAKE_C_COMPILER}\" ${_asm_flags}\n"
"          -I \"\${WG14_SIGFENCE_INCLUDE_DIR}\"\n"
"          \"\${CMAKE_CURRENT_SOURCE_DIR}/unfenced.c\"\n"
"  WORKING_DIRECTORY \"\${CMAKE_CURRENT_BINARY_DIR}\")\n"
"add_custom_target(probe_asm ALL\n"
"  DEPENDS fenced.${_asm_ext} unfenced.${_asm_ext})\n"
)

set(_build "${_work}/build")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${_work}" -B "${_build}"
          -DWG14_SIGFENCE_INCLUDE_DIR="${SRC_DIR}/include"
          ${_gen}
  RESULT_VARIABLE _rcfg OUTPUT_VARIABLE _cfgout ERROR_VARIABLE _cfgerr)
if(NOT _rcfg EQUAL 0)
  message(FATAL_ERROR "sigfence codegen configure failed (${_rcfg}): ${_cfgout} ${_cfgerr}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_build}" --parallel
  RESULT_VARIABLE _rbuild OUTPUT_VARIABLE _bldout ERROR_VARIABLE _blder)
if(NOT _rbuild EQUAL 0)
  message(FATAL_ERROR "sigfence codegen build failed (${_rbuild}): ${_bldout} ${_blder}")
endif()

file(READ "${_build}/fenced.${_asm_ext}" _fenced)
file(READ "${_build}/unfenced.${_asm_ext}" _unfenced)

# Stack-memory reference, AT&T ("(%rsp)"/"(%rbp)"), Intel/MSVC ("[rsp]"/"[rbp]")
# and ARM64 ("[sp, #..]").
set(_stack_re "\\(%r(sp|bp)\\)|\\[r(sp|bp)\\]|\\[sp")

if(_unfenced MATCHES "${_stack_re}")
  message(FATAL_ERROR
    "sigfence codegen baseline broken: the UNFENCED reference probe "
    "unexpectedly touches the stack, so the fenced/unfenced comparison cannot "
    "discriminate a real fence on this compiler. Assembly:\n${_unfenced}")
endif()
if(NOT _fenced MATCHES "${_stack_re}")
  message(FATAL_ERROR
    "sigfence() was optimized away: the FENCED probe's generated code "
    "contains no memory round-trip of the fenced local (plans/analysis.md "
    "2.9/W11). Assembly:\n${_fenced}")
endif()
message(STATUS
  "OK (analysis.md 2.9): sigfence() forces the fenced local through memory "
  "at Release optimization")
