# Cross-compile for 64-bit Windows with MinGW-w64 (Debian/Ubuntu:
# apt install gcc-mingw-w64-x86-64; either thread flavour works, as os.c
# uses Win32 threads directly).
#
# The Vulkan headers and the vulkan-1 import library come from the Vulkan
# SDK's Windows files, from MINGW_PREFIX, or from -DVulkan_INCLUDE_DIR and
# -DVulkan_LIBRARY (README.md, "Windows", makes the import library from the
# loader's vulkan-1.def with dlltool). glslc runs on the host. GLFW, mimalloc,
# SQLite and miniaudio are built from source (preset mingw64 sets
# MC_DEPS=FETCH). With Wine installed, ctest runs the unit tests and --bench.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MC_TRIPLE x86_64-w64-mingw32)

find_program(MC_MINGW_CC NAMES ${MC_TRIPLE}-gcc-posix ${MC_TRIPLE}-gcc ${MC_TRIPLE}-gcc-win32)
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

# Debian and Ubuntu's wine64 package keeps wine64 out of PATH.
find_program(MC_WINE NAMES wine64 wine HINTS /usr/lib/wine)
if(MC_WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${MC_WINE}")
endif()
