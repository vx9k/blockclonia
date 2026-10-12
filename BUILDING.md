# Building and running blockclonia

This page covers what you need, how to compile the game on each platform
it supports, how to start it, and what to do when it won't start. For
the command-line options and the controls, see the [README](README.md).

## What you need

**A GPU with a Vulkan driver.** Vulkan 1.0 with no optional features is
enough; the game uses newer features only where the driver offers them.
That covers any desktop GPU from the last ten years with current drivers,
Intel and AMD integrated graphics, and the Raspberry Pi 4 and 5 (Mesa's
V3DV driver). Without a GPU it runs on Mesa's software rasterizer,
lavapipe, slowly (see [No GPU](#no-gpu)).

**A supported system:**

| System | Status |
|---|---|
| Linux x86-64 (Debian, Ubuntu, Arch, ...) | Supported, tested in CI on every change |
| Raspberry Pi OS, 64-bit (Pi 4, Pi 5) | Supported, tested in CI, `.deb` packages built |
| Raspberry Pi OS, 32-bit | Supported, tested in CI under emulation |
| macOS on Apple Silicon | Experimental, through MoltenVK; built in CI, never played on a real Mac yet |
| Windows | Not supported |

**To build it:**

- a C11 compiler (gcc or clang), CMake 3.21 or newer and Ninja;
- the Vulkan headers and loader, and `glslc` (from shaderc) to compile
  the shaders;
- GLFW 3.5, mimalloc 3.5, miniaudio 0.11 and SQLite 3.31 or newer.

You don't have to install the last four yourself: when a system copy is
missing or older than the build wants, the build downloads and compiles
a pinned release (GLFW 3.5.1, mimalloc 3.5.3, miniaudio 0.11.25, SQLite
3.53.4). See [Dependencies](#dependencies) to control that.

## Linux

### Debian, Ubuntu, Raspberry Pi OS

```sh
sudo apt install build-essential cmake ninja-build glslc libvulkan-dev \
                 libglfw3-dev libmimalloc-dev libsqlite3-dev mesa-vulkan-drivers
cmake --preset native            # tuned for this machine (use this on a Pi)
cmake --build --preset native
./build/native/blockclonia
```

Most distributions still ship GLFW and mimalloc older than 3.5, so the
build downloads those. Compiling GLFW needs the X11 and Wayland
development files:

```sh
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
                 libxi-dev libwayland-dev libwayland-bin libxkbcommon-dev
```

### Arch Linux, CachyOS, Manjaro

Arch packages are recent enough that nothing has to be downloaded:

```sh
sudo pacman -S --needed base-devel cmake ninja shaderc vulkan-icd-loader \
                        vulkan-headers glfw mimalloc sqlite miniaudio
cmake --preset native
cmake --build --preset native
./build/native/blockclonia
```

Also install the Vulkan driver for your GPU: `vulkan-intel`,
`vulkan-radeon`, or NVIDIA's driver, which brings its own.

### Raspberry Pi

Build on the Pi itself with `--preset native`, as above. A Pi 4 needs a
few minutes for the first build. For a lighter load, start the game with
`--radius 4`.

32-bit Pi OS's compiler has NEON turned off by default; configure with
`-DMC_CPU=pi4` (or `pi5`) there.

To build for a Pi on a faster machine, use the cross-compiling presets
`pi4-aarch64`, `pi5-aarch64` or `pi4-armhf`. The header of
`cmake/toolchains/rpi-aarch64.cmake` explains the setup.

## macOS (Apple Silicon, experimental)

The renderer is written for Vulkan, which macOS doesn't have. MoltenVK
translates it to Metal, Apple's graphics API. Install the tools with
[Homebrew](https://brew.sh):

```sh
brew install cmake ninja mimalloc glfw vulkan-headers vulkan-loader molten-vk shaderc
cmake --preset macos -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build --preset macos
./build/macos/blockclonia
```

CI builds this and runs the unit tests on every change, but nobody has
played it on a Mac yet. If you try it, please report what happens,
working or not. A native Metal renderer is deferred; [TODO.md](TODO.md)
explains why.

## Build presets

`cmake --list-presets` lists them all. The ones you'll want:

| Preset | For |
|---|---|
| `native` | Playing: optimised for the CPU you build on |
| `release` | Packages: -O2 and link-time optimisation, runs on any CPU of the architecture |
| `dev` | Working on the code: debug build |
| `macos` | macOS with MoltenVK |
| `core` | Everything except the game itself; needs no Vulkan, GLFW or audio |
| `asan` | AddressSanitizer and UBSan |
| `profile` | Profiling |
| `fuzz` | libFuzzer targets for the file decoders (clang) |
| `ci-gcc`, `ci-clang` | What CI builds, with warnings as errors |

Options you can add when configuring, with `-D`:

- `MC_CPU=pi4|pi5|native|x86-64-v2` picks the CPU to tune for.
- `MC_SOUND=OFF` builds without sound, so miniaudio isn't needed.
- `MC_WERROR=ON` turns compiler warnings into errors.

## Dependencies

`MC_DEPS` decides where GLFW, mimalloc, miniaudio and SQLite come from:

- `AUTO` (the default) uses the system's copy when it is new enough and
  downloads the pinned release otherwise.
- `SYSTEM` never downloads. It also accepts GLFW 3.3 and mimalloc 2.1,
  for distribution packages, and builds a silent game when miniaudio is
  missing.
- `FETCH` always uses the pinned releases.

For a reproducible build, set `MC_MIMALLOC_SHA256`, `MC_GLFW_SHA256`,
`MC_MINIAUDIO_SHA256` and `MC_SQLITE_SHA256`; see
[SECURITY.md](SECURITY.md#supply-chain). `MC_SQLITE_VERSION` and
`MC_SQLITE_YEAR` select a different pinned SQLite release.

**Offline:** point `FETCHCONTENT_SOURCE_DIR_MIMALLOC`, `..._GLFW`,
`..._MINIAUDIO` and `..._SQLITE` at the `_deps/*-src` folders of an
earlier build, e.g.
`-DFETCHCONTENT_SOURCE_DIR_GLFW=$PWD/build/native/_deps/glfw-src`.

## Packages

```sh
cmake --preset release && cmake --build --preset release
cd build/release && cpack
```

This makes a `.tar.gz` and, on Linux, a `.deb`.

## Running

```sh
./build/native/blockclonia            # the title screen
./build/native/blockclonia --play     # straight into a world
./build/native/blockclonia --help     # every option
```

The world and settings are saved where your system keeps application
data; "Where files live" in the README has the exact paths. The
README also lists the controls and the command-line options.

### No GPU

Mesa's software rasterizer, lavapipe (`mesa-vulkan-drivers` on Debian,
`vulkan-swrast` on Arch), runs the game on the CPU. Select it with
`VK_ICD_FILENAMES`. On a machine without a display, wrap the command in
`xvfb-run` (`xvfb`, or `xorg-server-xvfb` on Arch):

```sh
xvfb-run -a env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/native/blockclonia --frames 300 --screenshot shot.ppm --no-save --no-sound
```

## Tests

```sh
cmake --preset dev && cmake --build --preset dev
ctest --preset dev               # unit tests, benchmarks, and a render test when lavapipe and xvfb are installed
./build/dev/mc_tests             # just the unit tests, a few seconds
tools/lint.sh                    # the lint checks CI runs (clang-tidy, cppcheck, clang-format 18)
```

## Troubleshooting

**`no Vulkan devices found` or `no Vulkan device can present to this window`.** Check the driver
with `vulkaninfo --summary` (from `vulkan-tools`). If it lists no GPU,
install your GPU's Vulkan driver, or use lavapipe as in
[No GPU](#no-gpu). With more than one GPU, `--gpu N` picks one.

**`glfwInit failed`.** There is no display: run it from a desktop
session, or under `xvfb-run`.

**Graphical glitches or crashes.** Run with `--validate` (needs the
Khronos validation layers: `vulkan-validationlayers` on Debian,
`vulkan-validation-layers` on Arch) and include the `[warn] vulkan:`
lines when you report it. `MC_VK_DISABLE=all` turns off every optional
Vulkan feature, and `MC_VK_API=1.0` caps the API version. If either one
fixes the problem, say so in the report: it narrows the bug down to one
feature.

**No sound.** The game prints a warning and continues without audio
when it can't open a sound device. `--no-sound` skips audio entirely.
