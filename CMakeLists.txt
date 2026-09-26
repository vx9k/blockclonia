# blockclonia build. CMake 3.21+ with Ninja is the reference setup; see
# CMakePresets.json for the supported configurations (`cmake --list-presets`).
cmake_minimum_required(VERSION 3.21...3.31)
project(blockclonia
  VERSION 0.1.0
  DESCRIPTION "Minecraft-style voxel sandbox for low-end hardware (C11, Vulkan 1.0)"
  LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # glibc feature macros (clock_gettime, mkdir)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(MC_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT MC_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- options
option(MC_BUILD_GAME  "Build the game (needs Vulkan headers/loader, GLFW and glslc)" ON)
option(MC_BUILD_TESTS "Build unit tests" ON)
option(MC_BUILD_FUZZ  "Build libFuzzer targets (clang only)" OFF)
option(MC_VK_MIMALLOC "Compile mimalloc-backed VkAllocationCallbacks (enabled at run time with MC_VK_ALLOC=1)" ON)
option(MC_SANITIZE    "Build with AddressSanitizer + UBSan" OFF)
option(MC_LTO         "Link-time optimisation in Release/MinSizeRel builds" ON)
option(MC_WERROR      "Treat compiler and shader warnings as errors (CI)" OFF)
option(MC_NATIVE      "Deprecated alias for MC_CPU=native" OFF)
set(MC_CPU "" CACHE STRING
  "CPU to optimise for: empty = compiler default, native, pi4, pi5, x86-64-v2, x86-64-v3")
set_property(CACHE MC_CPU PROPERTY STRINGS "" native pi4 pi5 x86-64-v2 x86-64-v3)
set(MC_DEPS AUTO CACHE STRING
  "Where GLFW and mimalloc come from: AUTO (system, else pinned download), SYSTEM, FETCH")
set_property(CACHE MC_DEPS PROPERTY STRINGS AUTO SYSTEM FETCH)

if(MC_BUILD_FUZZ AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MC_BUILD_FUZZ needs clang")
endif()

# ---------------------------------------------------------------- CPU flags
# Applied with add_compile_options so fetched dependencies (mimalloc, GLFW)
# get the same -mcpu as the game.
if(MC_NATIVE)
  set(MC_CPU native)
endif()
# Decide arm32 vs arm64 from the compiler, not the kernel: 32-bit Raspberry
# Pi OS on a Pi 4/5 runs a 64-bit kernel, so CMAKE_SYSTEM_PROCESSOR says
# "aarch64" while gcc produces armhf code.
set(MC_ARM32 OFF)
set(MC_ARM64 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|arm)")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MC_ARM64 ON)
  else()
    set(MC_ARM32 ON)
  endif()
endif()

set(MC_CPU_FLAGS "")
if(MC_CPU STREQUAL "")
elseif(MSVC)
  if(MC_CPU STREQUAL "x86-64-v3")
    set(MC_CPU_FLAGS /arch:AVX2)
  elseif(NOT MC_CPU STREQUAL "x86-64-v2")
    message(WARNING "MC_CPU=${MC_CPU} is ignored with MSVC")
  endif()
elseif(MC_CPU STREQUAL "native")
  if(MC_ARM32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # Raspberry Pi OS's gcc defaults to -mfpu=vfp; auto derives NEON from the CPU.
    set(MC_CPU_FLAGS -mcpu=native -mfpu=auto)
  elseif(MC_ARM32 OR MC_ARM64)
    set(MC_CPU_FLAGS -mcpu=native)
  else()
    set(MC_CPU_FLAGS -march=native)
  endif()
elseif(MC_CPU MATCHES "^pi[45]$")
  if(MC_CPU STREQUAL "pi4")
    set(_core cortex-a72)
  else()
    set(_core cortex-a76)
  endif()
  if(MC_ARM64)
    set(MC_CPU_FLAGS -mcpu=${_core})
  elseif(MC_ARM32)
    # V3DV (the Pi's Vulkan driver) only exists on Pi 4/5, so NEON is safe.
    set(MC_CPU_FLAGS -mcpu=${_core} -mfpu=neon-fp-armv8 -mfloat-abi=hard)
  else()
    message(FATAL_ERROR "MC_CPU=${MC_CPU} needs an ARM compiler (use a cmake/toolchains/rpi-*.cmake toolchain)")
  endif()
elseif(MC_CPU MATCHES "^x86-64-v[234]$")
  set(MC_CPU_FLAGS -march=${MC_CPU} -mtune=generic)
else()
  message(FATAL_ERROR "Unknown MC_CPU '${MC_CPU}'")
endif()
if(MC_CPU_FLAGS)
  message(STATUS "blockclonia: CPU flags ${MC_CPU_FLAGS}")
  add_compile_options(${MC_CPU_FLAGS})
  if(NOT MSVC)
    add_link_options(${MC_CPU_FLAGS}) # LTO code generation happens at link time
  endif()
endif()

# ---------------------------------------------------------------- dependencies
find_package(Threads REQUIRED)
if(MC_BUILD_GAME)
  find_package(Vulkan REQUIRED)
elseif(MC_VK_MIMALLOC)
  find_package(Vulkan QUIET) # headers only, for the allocation callbacks
endif()
include(Dependencies) # mc::mimalloc, and mc::glfw when MC_BUILD_GAME

# ---------------------------------------------------------------- LTO
# Set after the dependencies so only our own targets are affected, and for
# every one of them: a test linking the LTO-built core must use LTO too.
if(MC_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT MC_IPO_OK OUTPUT _ipo_msg LANGUAGES C)
  if(MC_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  else()
    message(STATUS "blockclonia: LTO not available (${_ipo_msg})")
  endif()
endif()

# ---------------------------------------------------------------- warnings
# Linked privately to our own targets, so fetched GLFW and mimalloc never
# see these flags. The gcc/clang set is in cmake/warnings.cmake.
add_library(mc_warnings INTERFACE)
if(MSVC)
  target_compile_options(mc_warnings INTERFACE /W4 $<$<BOOL:${MC_WERROR}>:/WX>)
else()
  include(warnings)
  target_compile_options(mc_warnings INTERFACE ${MC_WARNINGS})
endif()

# ---------------------------------------------------------------- core
# Everything that does not touch Vulkan or a window: the tests and the
# fuzzer link this alone.
set(MC_CORE_SOURCES
  src/bench.c
  src/block.c
  src/camera.c
  src/debug.c
  src/fx.c
  src/gpupool.c
  src/health.c
  src/hud.c
  src/interact.c
  src/inventory.c
  src/invui.c
  src/item.c
  src/jobs.c
  src/log.c
  src/mem.c
  src/menu.c
  src/mesher.c
  src/noise.c
  src/physics.c
  src/save.c
  src/settings.c
  src/survival.c
  src/texgen.c
  src/thermo.c
  src/ui.c
  src/viewmodel.c
  src/world.c
  src/worldgen.c
)

function(mc_add_core name)
  add_library(${name} STATIC ${MC_CORE_SOURCES})
  target_include_directories(${name} PUBLIC src)
  target_link_libraries(${name} PUBLIC Threads::Threads PRIVATE mc_warnings)
  if(NOT MC_SANITIZE)
    target_link_libraries(${name} PUBLIC mc::mimalloc)
  endif()
  if(NOT MSVC)
    target_link_libraries(${name} PUBLIC m)
  endif()
  if(MC_VK_MIMALLOC AND NOT MC_SANITIZE AND TARGET Vulkan::Headers)
    target_compile_definitions(${name} PRIVATE MC_VK_MIMALLOC) # only mem.c reads it
    target_link_libraries(${name} PRIVATE Vulkan::Headers)
  endif()
endfunction()

mc_add_core(mccore)

if(MC_SANITIZE)
  if(MSVC)
    target_compile_options(mccore PUBLIC /fsanitize=address)
  else()
    # -fno-sanitize-recover: a UBSan finding fails the test instead of
    # printing a line nobody reads.
    set(_san -fsanitize=address,undefined -fno-sanitize-recover=undefined)
    target_compile_options(mccore PUBLIC ${_san} -fno-omit-frame-pointer)
    target_link_options(mccore PUBLIC ${_san})
  endif()
  # ASan cannot see mi_* allocations, so sanitizer builds use libc malloc.
  target_compile_definitions(mccore PUBLIC MC_MEM_LIBC)
endif()

# ---------------------------------------------------------------- game
if(MC_BUILD_GAME)
  add_executable(blockclonia src/main.c src/renderer.c)
  target_link_libraries(blockclonia PRIVATE mccore Vulkan::Vulkan mc::glfw mc_warnings)

  include(Shaders)
  mc_compile_shaders(blockclonia
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    SOURCES block.vert block.frag entity.vert line.vert line.frag ui.vert ui.frag)
  mc_copy_runtime_dlls(blockclonia) # Windows: mimalloc DLLs next to the exe
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT blockclonia)
endif()

# ---------------------------------------------------------------- tests
if(MC_BUILD_TESTS)
  enable_testing()
  add_executable(mc_tests tests/test_main.c tests/test_util.c tests/test_thermo.c tests/test_health2.c
                          tests/test_bodies.c tests/test_visual.c)
  target_link_libraries(mc_tests PRIVATE mccore mc_warnings)
  add_test(NAME unit COMMAND mc_tests)
  set_tests_properties(unit PROPERTIES LABELS headless TIMEOUT 120)

  if(MC_BUILD_GAME)
    add_test(NAME bench COMMAND blockclonia --bench)
    set_tests_properties(bench PROPERTIES LABELS headless TIMEOUT 300)

    # A real frame loop on Mesa's software Vulkan driver under Xvfb, with the
    # validation layer: catches renderer regressions on machines without a GPU.
    find_program(MC_XVFB_RUN xvfb-run)
    # Debian names the ICD file per architecture (lvp_icd.aarch64.json).
    find_file(MC_LVP_ICD NAMES lvp_icd.json lvp_icd.${CMAKE_SYSTEM_PROCESSOR}.json
      PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d NO_DEFAULT_PATH)
    if(MC_XVFB_RUN AND MC_LVP_ICD AND NOT CMAKE_CROSSCOMPILING)
      add_test(NAME render_smoke
        COMMAND ${MC_XVFB_RUN} -a $<TARGET_FILE:blockclonia>
                --frames 120 --radius 4 --size 640x360 --seed 42 --no-save --validate)
      set_tests_properties(render_smoke PROPERTIES
        LABELS render TIMEOUT 120
        ENVIRONMENT "VK_ICD_FILENAMES=${MC_LVP_ICD}"
        FAIL_REGULAR_EXPRESSION "\\[(error|fatal)\\]|\\[warn\\] vulkan:|validation requested but")
    endif()
  endif()
endif()

if(MC_BUILD_FUZZ)
  # libFuzzer only learns from code compiled with coverage hooks, so the core
  # is built a second time with -fsanitize=fuzzer-no-link.
  mc_add_core(mccore_fuzz)
  target_compile_options(mccore_fuzz PUBLIC -fsanitize=fuzzer-no-link,address,undefined)
  add_executable(fuzz_save tests/fuzz_save.c)
  target_link_libraries(fuzz_save PRIVATE mccore_fuzz)
  target_link_options(fuzz_save PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

# ---------------------------------------------------------------- install / package
if(MC_BUILD_GAME)
  include(Packaging)
endif()
