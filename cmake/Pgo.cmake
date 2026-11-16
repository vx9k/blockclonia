# Profile-guided optimisation for the game and its core library (MC_PGO).
#
# PGO tells the compiler how the code really runs, so it can inline the hot
# calls, unroll the hot loops, lay out each function's likely branches first
# and push cold code aside. BOLT (Bolt.cmake) then improves the layout of the
# linked binary; the two stack, and PGO is the part that changes the code
# itself. The profile comes from `--bench` (world generation, meshing,
# physics), the same training run BOLT uses.
#
# It takes two compiles of the same sources. An instrumented copy of the
# core library and the game (blockclonia_pgogen, built with
# -fprofile-generate) runs `--bench`, llvm-profdata merges what it wrote,
# and the real mccore and blockclonia compile with -fprofile-use from that
# profile. The profile is built before them, so an edited file is always
# compiled against a fresh one; unedited files keep their objects, which
# were made with an older profile of the same code and stay correct.
#
# Linux, not cross-compiling (the build runs the instrumented game), release
# builds without sanitizers, and only with llvm-profdata and clang's profile
# runtime. AUTO turns it on where all that holds; ON fails the configure
# where it does not.

include(CheckCSourceCompiles)

function(mc_pgo core game)
  if(MC_PGO STREQUAL "OFF")
    return()
  endif()
  set(_why "")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_why "PGO is only wired up for Linux")
  elseif(CMAKE_CROSSCOMPILING)
    set(_why "a cross build cannot run the instrumented game")
  elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    set(_why "PGO is for release builds (this is ${CMAKE_BUILD_TYPE})")
  elseif(MC_SANITIZE)
    set(_why "sanitizer builds are not optimised")
  endif()

  if(NOT _why)
    # Paired with the compiler's version, since the profile format follows it.
    get_filename_component(_cc_name "${CMAKE_C_COMPILER}" NAME)
    string(REGEX MATCH "-[0-9]+$" _sfx "${_cc_name}")
    get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(MC_LLVM_PROFDATA NAMES llvm-profdata${_sfx} llvm-profdata HINTS "${_cc_dir}")
    if(NOT MC_LLVM_PROFDATA)
      set(_why "llvm-profdata not found (the llvm package)")
    else()
      set(CMAKE_REQUIRED_FLAGS -fprofile-generate)
      set(CMAKE_REQUIRED_LINK_OPTIONS -fprofile-generate)
      check_c_source_compiles("int main(void) { return 0; }" MC_HAVE_PROFILE_RUNTIME)
      if(NOT MC_HAVE_PROFILE_RUNTIME)
        set(_why "clang's profile runtime is missing (Debian/Ubuntu: libclang-rt-<version>-dev)")
      endif()
    endif()
  endif()

  if(_why)
    if(MC_PGO STREQUAL "ON")
      message(FATAL_ERROR "MC_PGO=ON, but ${_why}")
    endif()
    message(STATUS "blockclonia: PGO off: ${_why}")
    return()
  endif()
  message(STATUS "blockclonia: PGO on (${MC_LLVM_PROFDATA}), trained on --bench")

  # The instrumented copies: the same sources, definitions and libraries,
  # with the core swapped for its instrumented twin.
  mc_add_core(${core}_pgogen)
  target_compile_options(${core}_pgogen PUBLIC -fprofile-generate)
  target_link_options(${core}_pgogen PUBLIC -fprofile-generate)
  get_target_property(_srcs ${game} SOURCES)
  get_target_property(_defs ${game} COMPILE_DEFINITIONS)
  get_target_property(_incs ${game} INCLUDE_DIRECTORIES)
  get_target_property(_libs ${game} LINK_LIBRARIES)
  list(TRANSFORM _libs REPLACE "^${core}$" "${core}_pgogen")
  add_executable(${game}_pgogen ${_srcs})
  target_compile_definitions(${game}_pgogen PRIVATE ${_defs})
  target_include_directories(${game}_pgogen PRIVATE ${_incs})
  target_link_libraries(${game}_pgogen PRIVATE ${_libs})

  set(_work "${CMAKE_CURRENT_BINARY_DIR}/pgo")
  set(_profdata "${_work}/bench.profdata")
  add_custom_command(OUTPUT "${_profdata}"
    COMMAND "${CMAKE_COMMAND}" "-DPROFDATA_TOOL=${MC_LLVM_PROFDATA}" "-DEXE=$<TARGET_FILE:${game}_pgogen>"
            "-DWORK=${_work}" "-DOUT=${_profdata}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PgoRun.cmake"
    DEPENDS ${game}_pgogen "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PgoRun.cmake"
    COMMENT "PGO: profiling ${game} on --bench"
    VERBATIM)
  add_custom_target(${game}_pgo_profile DEPENDS "${_profdata}")

  # The mesher is left out: with the profile, the gather+mesh benchmark ran
  # 16% slower (0.103 against 0.089 ms/section, clang 23, x86-64), with
  # the same inlining but about 100 M more instructions in
  # mesh_section_counts, whose hand-tuned loops the profile only makes
  # worse. Compiled without it, meshing is as fast as without PGO.
  set_source_files_properties(src/mesher.c DIRECTORY ${CMAKE_SOURCE_DIR}
    PROPERTIES COMPILE_OPTIONS -fno-profile-instr-use)
  # PRIVATE: the unit tests link the core but keep their own flags.
  foreach(_t IN ITEMS ${core} ${game})
    add_dependencies(${_t} ${game}_pgo_profile)
    target_compile_options(${_t} PRIVATE "-fprofile-use=${_profdata}")
  endforeach()
endfunction()
