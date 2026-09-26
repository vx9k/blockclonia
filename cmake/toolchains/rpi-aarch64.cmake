# Cross-compile for 64-bit Raspberry Pi OS (Pi 4: Cortex-A72, Pi 5: Cortex-A76)
# and other arm64 Debian-family boards.
#
#   RPI_SYSROOT=~/pi-sysroot cmake --preset pi4-aarch64      (gcc cross)
#   RPI_SYSROOT=~/pi-sysroot RPI_COMPILER=clang cmake --preset pi4-aarch64
#
# Without a sysroot (Debian bookworm matches Raspberry Pi OS bookworm):
#   dpkg --add-architecture arm64 && apt update
#   apt install crossbuild-essential-arm64 libvulkan-dev:arm64 \
#               libglfw3-dev:arm64 libmimalloc-dev:arm64 glslc qemu-user
#   cmake --preset pi4-aarch64
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(MC_TRIPLE aarch64-linux-gnu)
set(MC_QEMU aarch64)
include(${CMAKE_CURRENT_LIST_DIR}/rpi-common.cmake)
