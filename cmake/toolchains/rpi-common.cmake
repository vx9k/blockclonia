# Shared part of rpi-aarch64.cmake and rpi-armhf.cmake (which set MC_TRIPLE
# and MC_QEMU first). Settings come from environment variables because CMake
# re-reads toolchain files inside try_compile projects, where the cache of the
# main project is not visible.
#
#   RPI_SYSROOT   copy of the Pi's root filesystem. Leave unset to use Debian/
#                 Ubuntu multiarch packages (libfoo-dev:arm64) instead.
#   RPI_COMPILER  "gcc" (default: <triple>-gcc) or "clang" (clang --target
#                 plus lld: no cross binutils needed, only the sysroot).
#
# Making a sysroot. On the Pi, first:
#   sudo apt install build-essential libvulkan-dev libglfw3-dev libmimalloc-dev
# then on the build machine:
#   rsync -aR pi@raspberrypi:/usr/include pi@raspberrypi:/usr/lib \
#         pi@raspberrypi:/lib ~/pi-sysroot/
#   symlinks -rc ~/pi-sysroot     # (package "symlinks") make absolute links relative

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_LIBRARY_ARCHITECTURE ${MC_TRIPLE}) # Debian multiarch lib/<triple>

if(DEFINED ENV{RPI_SYSROOT} AND NOT "$ENV{RPI_SYSROOT}" STREQUAL "")
  set(CMAKE_SYSROOT "$ENV{RPI_SYSROOT}")
endif()

if("$ENV{RPI_COMPILER}" STREQUAL "clang")
  set(CMAKE_C_COMPILER clang)
  set(CMAKE_C_COMPILER_TARGET ${MC_TRIPLE})
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
  # The host's GNU ar cannot index ARM objects; llvm-ar can.
  find_program(MC_LLVM_AR NAMES llvm-ar llvm-ar-19 llvm-ar-18 llvm-ar-17 llvm-ar-16 llvm-ar-15)
  find_program(MC_LLVM_RANLIB NAMES llvm-ranlib llvm-ranlib-19 llvm-ranlib-18 llvm-ranlib-17 llvm-ranlib-16 llvm-ranlib-15)
  if(MC_LLVM_AR)
    set(CMAKE_AR "${MC_LLVM_AR}" CACHE FILEPATH "")
    set(CMAKE_RANLIB "${MC_LLVM_RANLIB}" CACHE FILEPATH "")
  endif()
else()
  set(CMAKE_C_COMPILER ${MC_TRIPLE}-gcc)
endif()

# Libraries, headers and CMake packages come from the target; programs
# (glslc, dpkg tools) from the build machine.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Default CPU for these toolchains; the presets and -DMC_CPU=... override it.
set(MC_CPU pi4 CACHE STRING "CPU to optimise for")

# Run the tests under qemu-user when it is installed (ctest uses this
# automatically for add_test commands that name a target).
find_program(MC_QEMU_PROGRAM NAMES qemu-${MC_QEMU} qemu-${MC_QEMU}-static)
if(MC_QEMU_PROGRAM)
  if(CMAKE_SYSROOT)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${MC_QEMU_PROGRAM};-L;${CMAKE_SYSROOT}")
  else()
    set(CMAKE_CROSSCOMPILING_EMULATOR "${MC_QEMU_PROGRAM}")
  endif()
endif()
