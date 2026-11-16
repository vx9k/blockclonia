# LLVM BOLT for the game binary (MC_BOLT).
#
# BOLT rewrites a linked binary from a profile of how it ran: hot basic
# blocks laid out together, cold code split off, hot functions grouped so
# they share instruction-cache lines and TLB pages. This is the part of the
# win that LTO cannot get, because the compiler does not know which branches
# are taken. The profile comes from an instrumented copy run through
# `--bench` (world generation, meshing, physics: the CPU-bound work behind
# frame time), so it needs no perf, LBR or GPU and works on CI runners.
#
# Linux only (BOLT reads and writes ELF), x86-64 and arm64 only (it has no
# 32-bit ARM support), and not when cross-compiling, since the build has to
# run the binary to profile it. AUTO turns it on where all that holds and
# llvm-bolt 18 or newer is found; ON fails the configure where it does not.
#
# After linking, the build replaces the game binary with the optimised one
# and keeps the original as blockclonia.prebolt, for comparing the two with
# `--bench`.

function(mc_bolt target)
  if(MC_BOLT STREQUAL "OFF")
    return()
  endif()
  set(_why "")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_why "BOLT only rewrites ELF binaries (Linux)")
  elseif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64)$")
    set(_why "BOLT supports x86-64 and arm64, not ${CMAKE_SYSTEM_PROCESSOR}")
  elseif(CMAKE_CROSSCOMPILING)
    set(_why "a cross build cannot run the binary to profile it")
  elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    set(_why "BOLT is for release builds (this is ${CMAKE_BUILD_TYPE})")
  elseif(MC_SANITIZE)
    set(_why "sanitizer builds are not optimised")
  endif()

  if(NOT _why)
    # Paired with the compiler's version when it has a suffix (clang-19).
    get_filename_component(_cc_name "${CMAKE_C_COMPILER}" NAME)
    string(REGEX MATCH "-[0-9]+$" _sfx "${_cc_name}")
    get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    file(GLOB _llvm_dirs LIST_DIRECTORIES true "/usr/lib/llvm-*/bin")
    list(SORT _llvm_dirs COMPARE NATURAL ORDER DESCENDING)
    find_program(MC_LLVM_BOLT NAMES llvm-bolt${_sfx} llvm-bolt HINTS "${_cc_dir}" ${_llvm_dirs})
    if(NOT MC_LLVM_BOLT)
      set(_why "llvm-bolt not found (Debian/Ubuntu: the bolt or bolt-<version> package)")
    else()
      execute_process(COMMAND "${MC_LLVM_BOLT}" --version OUTPUT_VARIABLE _ver ERROR_QUIET)
      string(REGEX MATCH "LLVM version ([0-9]+)" _m "${_ver}")
      # -reorder-functions=cdsort arrived in LLVM 18.
      if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 LESS 18)
        set(_why "llvm-bolt ${CMAKE_MATCH_1} is older than 18")
      endif()
    endif()
  endif()

  if(_why)
    if(MC_BOLT STREQUAL "ON")
      message(FATAL_ERROR "MC_BOLT=ON, but ${_why}")
    endif()
    message(STATUS "blockclonia: BOLT off: ${_why}")
    return()
  endif()
  message(STATUS "blockclonia: BOLT on (${MC_LLVM_BOLT}), trained on --bench")

  # BOLT moves functions only when it can see every reference to them, so
  # the linker keeps the relocations in the output (lld --emit-relocs).
  target_link_options(${target} PRIVATE "LINKER:--emit-relocs")
  # Relink, and so optimise again, when the BOLT step itself changes.
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BoltRun.cmake")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DBOLT=${MC_LLVM_BOLT}" "-DEXE=$<TARGET_FILE:${target}>"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/bolt" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BoltRun.cmake"
    COMMENT "BOLT: profiling ${target} on --bench and optimising it"
    VERBATIM)
endfunction()
