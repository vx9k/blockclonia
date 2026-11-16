# Shared part of rpi-aarch64.cmake and rpi-armhf.cmake (which set MC_TRIPLE
# and MC_QEMU first). Settings come from environment variables because CMake
# re-reads toolchain files inside try_compile projects, where the cache of the
# main project is not visible.
#
#   RPI_SYSROOT   copy of the Pi's root filesystem. Leave unset to use Debian/
#                 Ubuntu multiarch packages (libfoo-dev:arm64) instead.
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

# pkg-config runs on the build machine and would otherwise read the build
# machine's .pc files; GLFW built from source finds wayland-client and
# xkbcommon through it. Point it at the target's, unless the caller has.
if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
  set(ENV{PKG_CONFIG_LIBDIR}
      "${CMAKE_SYSROOT}/usr/lib/${MC_TRIPLE}/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
  if(CMAKE_SYSROOT)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
  endif()
endif()

# clang cross-compiles with --target and links with lld, so no cross
# binutils are needed: only the target's headers, libraries and C runtime
# (the sysroot, or libc6-dev:<arch> and libgcc-<n>-dev:<arch>).
set(CMAKE_C_COMPILER_TARGET ${MC_TRIPLE})
include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)

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
