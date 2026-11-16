# Cross-compile for 32-bit Raspberry Pi OS (armhf) on a Pi 4 or Pi 5.
#
# Raspberry Pi OS's own armhf userland is built for ARMv6 + VFPv2 (Pi 1/Zero),
# so a native build there gets no NEON unless told to. Only the Pi 4 and 5
# have a Vulkan driver (V3DV), so MC_CPU=pi4 (the default) targets
# ARMv8-A AArch32 with NEON: -mcpu=cortex-a72 -mfpu=neon-fp-armv8
# -mfloat-abi=hard. The hard-float ABI is the same as Pi OS's, so the result
# links against its libraries.
#
#   RPI_SYSROOT=~/pi-sysroot-armhf cmake --preset pi4-armhf
#
# Without a sysroot (Debian trixie):
#   dpkg --add-architecture armhf && apt update
#   apt install clang lld llvm libc6-dev:armhf libgcc-14-dev:armhf \
#               libvulkan-dev:armhf libglfw3-dev:armhf libmimalloc-dev:armhf \
#               glslc qemu-user
#
# BOLT does not handle 32-bit ARM, so this build is clang + lld + LTO only.
set(CMAKE_SYSTEM_PROCESSOR arm)
set(MC_TRIPLE arm-linux-gnueabihf)
set(MC_QEMU arm)
# Debian armhf (trixie, and Pi OS on it) uses 64-bit time_t and off_t, and
# its gcc turns that on by default; clang does not. Without these the game
# disagrees with the system libraries about struct layouts, and readdir and
# stat fail with EOVERFLOW on 64-bit inode numbers.
set(CMAKE_C_FLAGS_INIT "-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64")
include(${CMAKE_CURRENT_LIST_DIR}/rpi-common.cmake)
