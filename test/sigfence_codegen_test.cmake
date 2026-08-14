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

# The probes deliberately do NOT enable header-only mode: the sigfence() macros
# are independent of it, and in header-only mode the include chain also pulls in
# static __attribute__((constructor)) functions (synchronous_sigset etc.) that
# legitimately use the stack -- which would trip the whole-file stack-traffic
# assertions below (Linux CI, 2026-08-10).
file(WRITE "${_work}/fenced.c"
"#include \"wg14_signals/thrd_signal_handle.h\"\n"
"int sigfence_probe(int input)\n"
"{\n"
"  int a = input * 3 + 1;\n"
"  sigfence(a);\n"
"  return a + 1;\n"
"}\n")
file(WRITE "${_work}/unfenced.c"
"#include \"wg14_signals/thrd_signal_handle.h\"\n"
"int sigfence_probe_unfenced(int input)\n"
"{\n"
"  int a = input * 3 + 1;\n"
"  return a + 1;\n"
"}\n")

if(COMPILER_ID STREQUAL "MSVC")
  # /std:c11 and /experimental:c11atomics match the library's own MSVC build
  # (CMake's `c_std_11` feature + /experimental:c11atomics). /Fa names the
  # listing path explicitly: without it cl writes the .asm wherever it pleases
  # (next to the source), not where this script reads it back (Windows CI,
  # 2026-08-10).
  set(_asm_ext "asm")
  # /Zc:__VAOPT__ (plans/analysis.md 4.10): sigfence()'s argument counting needs
  # __VA_OPT__, which pre-17.9 MSVC only enables with the flag even in C11 mode.
  set(_asm_flags "/FAsc /c /O2 /std:c11 /experimental:c11atomics /Zc:__VAOPT__")
  set(_asm_out_flag "/Fa")
else()
  # gnu11, not strict c11: the library and its consumers are built by CMake's
  # `c_std_11` feature, which defaults to GNU extensions on. Strict `-std=c11`
  # hides POSIX types (sigset_t, ucontext_t, NSIG, struct sigaction) under
  # glibc, which breaks the probe exactly as it breaks real strict-C11
  # consumers (see plans/ideas.md 2.3).
  set(_asm_ext "s")
  set(_asm_flags "-std=gnu11 -O3 -S")
  set(_asm_out_flag "-o")
endif()

# A throwaway project whose only job is to compile the two probes to assembly
# via the real toolchain (so the MSVC environment is set up correctly on
# Windows). The listing output path is named explicitly ({_asm_out_flag}),
# so the .s/.asm always lands where this script reads it back, independent of
# the compiler's working-directory conventions. No DEPENDS on the probe files:
# the outer script recreates this whole directory on every run, so the outputs
# never persist and the commands always run.
file(WRITE "${_work}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.15)\n"
"project(sigfence_codegen_probe LANGUAGES C)\n"
"\n"
"add_custom_command(\n"
"  OUTPUT \"\${CMAKE_CURRENT_BINARY_DIR}/fenced.${_asm_ext}\"\n"
"  COMMAND \"\${CMAKE_C_COMPILER}\" ${_asm_flags}\n"
"          ${_asm_out_flag}\"\${CMAKE_CURRENT_BINARY_DIR}/fenced.${_asm_ext}\"\n"
"          -I \"\${WG14_SIGFENCE_INCLUDE_DIR}\"\n"
"          \"\${CMAKE_CURRENT_SOURCE_DIR}/fenced.c\"\n"
"  WORKING_DIRECTORY \"\${CMAKE_CURRENT_BINARY_DIR}\")\n"
"add_custom_command(\n"
"  OUTPUT \"\${CMAKE_CURRENT_BINARY_DIR}/unfenced.${_asm_ext}\"\n"
"  COMMAND \"\${CMAKE_C_COMPILER}\" ${_asm_flags}\n"
"          ${_asm_out_flag}\"\${CMAKE_CURRENT_BINARY_DIR}/unfenced.${_asm_ext}\"\n"
"          -I \"\${WG14_SIGFENCE_INCLUDE_DIR}\"\n"
"          \"\${CMAKE_CURRENT_SOURCE_DIR}/unfenced.c\"\n"
"  WORKING_DIRECTORY \"\${CMAKE_CURRENT_BINARY_DIR}\")\n"
"add_custom_target(probe_asm ALL\n"
"  DEPENDS \"\${CMAKE_CURRENT_BINARY_DIR}/fenced.${_asm_ext}\"\n"
"          \"\${CMAKE_CURRENT_BINARY_DIR}/unfenced.${_asm_ext}\")\n"
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
