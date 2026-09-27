# Cross-compile for 64-bit Windows with MinGW-w64 (Debian/Ubuntu:
# apt install gcc-mingw-w64-x86-64-posix; the posix flavour provides
# winpthreads, which jobs.c needs).
#
# The Vulkan loader and headers come from the Vulkan SDK's Windows files or a
# MinGW build of Vulkan-Loader under MINGW_PREFIX; glslc runs on the host.
# GLFW and mimalloc are built from source (preset mingw64 sets MC_DEPS=FETCH).
#
# Experimental: the game does not compile for Windows yet (mkdir with a mode
# argument, rename() onto an existing file in save.c).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MC_TRIPLE x86_64-w64-mingw32)

find_program(MC_MINGW_CC NAMES ${MC_TRIPLE}-gcc-posix ${MC_TRIPLE}-gcc)
set(CMAKE_C_COMPILER ${MC_MINGW_CC})
set(CMAKE_RC_COMPILER ${MC_TRIPLE}-windres)

if(DEFINED ENV{MINGW_PREFIX})
  set(CMAKE_FIND_ROOT_PATH "$ENV{MINGW_PREFIX}" /usr/${MC_TRIPLE})
else()
  set(CMAKE_FIND_ROOT_PATH /usr/${MC_TRIPLE})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Static libgcc/winpthreads: the .exe then needs no MinGW DLLs next to it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

find_program(MC_WINE NAMES wine64 wine)
if(MC_WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${MC_WINE}")
endif()
