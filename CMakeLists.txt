cmake_minimum_required(VERSION 3.21)
project(blockclonia VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # glibc feature macros (clock_gettime, mkdir)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MC_VK_MIMALLOC "Route Vulkan driver host allocations through mimalloc" ON)
option(MC_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(MC_NATIVE "Tune for the build machine's CPU (-march=native)" OFF)
option(MC_BUILD_TESTS "Build unit tests" ON)
option(MC_BUILD_FUZZ "Build libFuzzer targets (clang only)" OFF)

find_package(Threads REQUIRED)
find_package(mimalloc 2.0 REQUIRED)

set(MC_WARNINGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wformat=2)

# ---------------------------------------------------------------- core
# Everything that does not touch Vulkan or a window: tests and the fuzzer
# link this alone.
add_library(mccore STATIC
  src/bench.c
  src/block.c
  src/gpupool.c
  src/jobs.c
  src/mem.c
  src/mesher.c
  src/noise.c
  src/physics.c
  src/save.c
  src/texgen.c
  src/world.c
  src/worldgen.c
)
target_include_directories(mccore PUBLIC src)
target_compile_options(mccore PRIVATE ${MC_WARNINGS})
# mimalloc goes first so its malloc/free take precedence for the process.
target_link_libraries(mccore PUBLIC mimalloc Threads::Threads m)

if(MC_SANITIZE)
  target_compile_options(mccore PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(mccore PUBLIC -fsanitize=address,undefined)
endif()
if(MC_NATIVE)
  target_compile_options(mccore PUBLIC -march=native)
endif()

# ---------------------------------------------------------------- game
find_package(Vulkan REQUIRED)
find_package(glfw3 3.3 REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(shader block.vert block.frag entity.vert line.vert line.frag)
  set(out ${SHADER_OUT}/${shader}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.0 -mfmt=c -I ${SHADER_DIR} -o ${out} ${SHADER_DIR}/${shader}
    DEPENDS ${SHADER_DIR}/${shader} ${SHADER_DIR}/common.glsl
    COMMENT "Compiling shader ${shader}"
    VERBATIM)
  list(APPEND SHADER_INCS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_INCS})

add_executable(blockclonia src/main.c src/renderer.c)
add_dependencies(blockclonia shaders)
target_include_directories(blockclonia PRIVATE ${SHADER_OUT})
target_compile_options(blockclonia PRIVATE ${MC_WARNINGS})
target_link_libraries(blockclonia PRIVATE mccore Vulkan::Vulkan glfw)
if(MC_VK_MIMALLOC)
  target_compile_definitions(mccore PUBLIC MC_VK_MIMALLOC)
  target_link_libraries(mccore PUBLIC Vulkan::Headers)
endif()

# ---------------------------------------------------------------- tests
if(MC_BUILD_TESTS)
  enable_testing()
  add_executable(mc_tests tests/test_main.c)
  target_compile_options(mc_tests PRIVATE ${MC_WARNINGS})
  target_link_libraries(mc_tests PRIVATE mccore)
  add_test(NAME unit COMMAND mc_tests)
  add_test(NAME bench COMMAND blockclonia --bench)
endif()

if(MC_BUILD_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MC_BUILD_FUZZ needs clang")
  endif()
  add_executable(fuzz_save tests/fuzz_save.c)
  target_compile_options(fuzz_save PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_save PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_save PRIVATE mccore)
endif()
