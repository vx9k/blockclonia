# The LLVM toolchain blockclonia builds with: clang, lld and LLVM's
# binutils (llvm-ar, llvm-ranlib, llvm-nm, llvm-objcopy, llvm-strip).
# CMakeLists.txt uses it whenever no other toolchain file is given.
#
# One toolchain everywhere means one set of warnings and one LTO format:
# clang's ThinLTO bitcode needs LLVM's archiver and linker, which is why ar,
# ranlib and the linker are not left to the system defaults. Versioned
# names (clang-19, llvm-ar-19) are found too, for distributions that install
# LLVM that way. -DCMAKE_C_COMPILER=clang-19 picks a specific version, and
# the binutils search then prefers the same version.
#
# macOS keeps Apple's linker and archiver: Xcode's clang is LLVM, but
# ld64.lld is not part of it, and BOLT (cmake/Bolt.cmake) is ELF-only.

if(NOT CMAKE_C_COMPILER)
  find_program(MC_CLANG NAMES clang clang-23 clang-22 clang-21 clang-20 clang-19 clang-18 clang-17 clang-16 clang-15 clang-14)
  if(NOT MC_CLANG)
    message(FATAL_ERROR "blockclonia builds with LLVM: install clang, lld and llvm (see BUILDING.md)")
  endif()
  set(CMAKE_C_COMPILER "${MC_CLANG}")
endif()
get_filename_component(_mc_cc_name "${CMAKE_C_COMPILER}" NAME)
if(NOT _mc_cc_name MATCHES "clang")
  message(FATAL_ERROR "blockclonia builds with LLVM: CMAKE_C_COMPILER must be clang, not ${_mc_cc_name}")
endif()

if(NOT CMAKE_HOST_APPLE)
  # The version suffix of the compiler, if it has one ("-19" for clang-19),
  # so a versioned compiler is paired with the same LLVM's tools.
  string(REGEX MATCH "-[0-9]+$" _mc_llvm_suffix "${_mc_cc_name}")
  get_filename_component(_mc_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
  foreach(_tool ar ranlib nm objcopy objdump strip readelf)
    string(TOUPPER "${_tool}" _var)
    find_program(MC_LLVM_${_var} NAMES llvm-${_tool}${_mc_llvm_suffix} llvm-${_tool}
                 HINTS "${_mc_cc_dir}" NO_CMAKE_FIND_ROOT_PATH)
    if(NOT MC_LLVM_${_var})
      message(FATAL_ERROR "llvm-${_tool} not found: install LLVM's binutils (Debian/Ubuntu: llvm)")
    endif()
    set(CMAKE_${_var} "${MC_LLVM_${_var}}" CACHE FILEPATH "" FORCE)
  endforeach()
  # The archiver CMake uses for LTO objects.
  set(CMAKE_C_COMPILER_AR "${MC_LLVM_AR}" CACHE FILEPATH "" FORCE)
  set(CMAKE_C_COMPILER_RANLIB "${MC_LLVM_RANLIB}" CACHE FILEPATH "" FORCE)

  find_program(MC_LLD NAMES ld.lld${_mc_llvm_suffix} ld.lld HINTS "${_mc_cc_dir}" NO_CMAKE_FIND_ROOT_PATH)
  if(NOT MC_LLD)
    message(FATAL_ERROR "ld.lld not found: install lld")
  endif()
  # The full path, so clang uses this lld and not another one on PATH.
  set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${MC_LLD}")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "--ld-path=${MC_LLD}")
  set(CMAKE_MODULE_LINKER_FLAGS_INIT "--ld-path=${MC_LLD}")
endif()
