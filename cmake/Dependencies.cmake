# Third-party libraries.
#
# Vulkan (headers, loader, glslc) always comes from the system or the Vulkan
# SDK. GLFW and mimalloc come from, depending on MC_DEPS:
#   AUTO    the system (or vcpkg/Conan/Homebrew via CMAKE_PREFIX_PATH or a
#           toolchain file) if a usable version is found, else the pinned
#           release below, downloaded and built as part of this project
#   SYSTEM  the system only; never touches the network (distro packaging)
#   FETCH   always the pinned release (Windows/macOS builds, portable Linux
#           tarballs, reproducible CI)
#
# Provides the interface targets mc::mimalloc and mc::glfw.

include(FetchContent)

# Pinned releases. To pin by content as well, set the *_SHA256 variables to
# the archive's hash (sha256sum of the downloaded tarball).
set(MC_MIMALLOC_VERSION 2.1.7 CACHE STRING "mimalloc release used when fetching")
set(MC_MIMALLOC_SHA256 "" CACHE STRING "Optional SHA256 of the mimalloc archive")
set(MC_GLFW_VERSION 3.4 CACHE STRING "GLFW release used when fetching")
set(MC_GLFW_SHA256 "" CACHE STRING "Optional SHA256 of the GLFW archive")

if(POLICY CMP0135) # CMake 3.24+: extracted files get the extraction time
  cmake_policy(SET CMP0135 NEW)
endif()

if(NOT MC_DEPS MATCHES "^(AUTO|SYSTEM|FETCH)$")
  message(FATAL_ERROR "MC_DEPS must be AUTO, SYSTEM or FETCH (got '${MC_DEPS}')")
endif()

function(_mc_hash_arg out sha)
  if(sha)
    set(${out} URL_HASH SHA256=${sha} PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------- mimalloc
# How the override of malloc (for GLFW, the Vulkan loader and the driver)
# works per platform:
#   Linux/BSD  shared libmimalloc (system package) or static with MI_OVERRIDE:
#              either way the process's malloc symbol resolves to mimalloc,
#              including in drivers the Vulkan loader dlopen()s later.
#   Windows    (MSVC) only the DLL build with mimalloc-redirect.dll next to
#              the exe patches the CRT's malloc, and only with the dynamic
#              CRT (/MD, CMake's default).
#   MinGW      no override; the game's own mi_* calls still use mimalloc.
#   macOS      static build registers mimalloc as the default malloc zone
#              (expected from mimalloc's docs; not yet tested here).
#   Android    no override is possible (bionic); explicit mi_* only.
# Where there is no override, VkAllocationCallbacks (MC_VK_ALLOC=1) and
# glfwInitAllocator (GLFW 3.4+) are the way to reach third-party code.
add_library(mc_mimalloc INTERFACE)
add_library(mc::mimalloc ALIAS mc_mimalloc)
set(MC_MIMALLOC_DLL "") # Windows: DLLs to ship next to the exe

if(NOT MC_DEPS STREQUAL "FETCH")
  # mem.c uses mi_option_arena_reserve and mi_option_arena_eager_commit,
  # which appeared in mimalloc 2.1.0 (2.0.x named the latter
  # eager_region_commit). Where the distro ships 2.0.x, AUTO builds the
  # pinned release instead.
  # No version in the call: mimalloc's version file rejects any other major
  # version, which would also hide a usable 3.x. Checked by hand instead.
  find_package(mimalloc CONFIG QUIET)
  if(mimalloc_FOUND AND mimalloc_VERSION VERSION_LESS 2.1)
    message(STATUS "blockclonia: system mimalloc ${mimalloc_VERSION} is too old (need 2.1+)")
    set(mimalloc_FOUND FALSE)
  endif()
endif()

if(mimalloc_FOUND AND (TARGET mimalloc OR TARGET mimalloc-static))
  if(TARGET mimalloc AND NOT ANDROID)
    target_link_libraries(mc_mimalloc INTERFACE mimalloc)        # shared: overrides malloc
  else()
    target_link_libraries(mc_mimalloc INTERFACE mimalloc-static)
  endif()
  message(STATUS "blockclonia: mimalloc ${mimalloc_VERSION} (system)")
elseif(MC_DEPS STREQUAL "SYSTEM")
  message(FATAL_ERROR "mimalloc 2.1+ not found and MC_DEPS=SYSTEM. Install it "
                      "(Debian/Ubuntu: libmimalloc-dev) or use MC_DEPS=AUTO.")
else()
  # Variables set inside a function only reach the subproject added from it;
  # CMP0077 makes them override mimalloc's option() defaults.
  function(_mc_fetch_mimalloc)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(MI_BUILD_TESTS OFF)
    set(MI_BUILD_OBJECT OFF)
    # Newer mimalloc releases can add -march=armv8.1-a (LSE atomics) on
    # arm64, which faults on the Pi 4's Cortex-A72 (ARMv8.0). MC_CPU decides.
    set(MI_OPT_ARCH OFF)
    if(MSVC)
      set(MI_BUILD_SHARED ON)
      set(MI_BUILD_STATIC OFF)
      set(MI_OVERRIDE ON)
    elseif(ANDROID OR MINGW)
      set(MI_BUILD_SHARED OFF)
      set(MI_BUILD_STATIC ON)
      set(MI_OVERRIDE OFF)
    else()
      set(MI_BUILD_SHARED OFF)
      set(MI_BUILD_STATIC ON)
      set(MI_OVERRIDE ON)
    endif()
    _mc_hash_arg(hash "${MC_MIMALLOC_SHA256}")
    FetchContent_Declare(mimalloc
      URL https://github.com/microsoft/mimalloc/archive/refs/tags/v${MC_MIMALLOC_VERSION}.tar.gz
      ${hash})
    FetchContent_MakeAvailable(mimalloc)
  endfunction()
  _mc_fetch_mimalloc()
  if(MSVC)
    target_link_libraries(mc_mimalloc INTERFACE mimalloc)
    # Load mimalloc.dll before any other DLL allocates.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      target_link_options(mc_mimalloc INTERFACE /INCLUDE:_mi_version)
    else()
      target_link_options(mc_mimalloc INTERFACE /INCLUDE:mi_version)
    endif()
    FetchContent_GetProperties(mimalloc SOURCE_DIR _mi_src)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_redirect "${_mi_src}/bin/mimalloc-redirect32.dll")
    else()
      set(_redirect "${_mi_src}/bin/mimalloc-redirect.dll")
    endif()
    set(MC_MIMALLOC_DLL "$<TARGET_FILE:mimalloc>;${_redirect}")
  else()
    target_link_libraries(mc_mimalloc INTERFACE mimalloc-static)
  endif()
  message(STATUS "blockclonia: mimalloc ${MC_MIMALLOC_VERSION} (built from source)")
endif()

# Copies the mimalloc DLLs next to an executable (Windows only; no-op elsewhere).
function(mc_copy_runtime_dlls target)
  if(MC_MIMALLOC_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MC_MIMALLOC_DLL} $<TARGET_FILE_DIR:${target}>
      COMMAND_EXPAND_LISTS VERBATIM)
  endif()
endfunction()

# ---------------------------------------------------------------- GLFW
if(MC_BUILD_GAME)
  add_library(mc_glfw INTERFACE)
  add_library(mc::glfw ALIAS mc_glfw)

  if(NOT MC_DEPS STREQUAL "FETCH")
    find_package(glfw3 3.3 CONFIG QUIET)
  endif()

  if(glfw3_FOUND)
    target_link_libraries(mc_glfw INTERFACE glfw)
    message(STATUS "blockclonia: GLFW ${glfw3_VERSION} (system)")
  elseif(MC_DEPS STREQUAL "SYSTEM")
    message(FATAL_ERROR "GLFW 3.3+ not found and MC_DEPS=SYSTEM. Install it "
                        "(Debian/Ubuntu: libglfw3-dev) or use MC_DEPS=AUTO.")
  else()
    # On Linux, GLFW 3.4 builds both X11 and Wayland back ends and picks one
    # at run time; that needs the X11 and Wayland development packages.
    function(_mc_fetch_glfw)
      set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
      set(BUILD_SHARED_LIBS OFF)
      set(GLFW_BUILD_EXAMPLES OFF)
      set(GLFW_BUILD_TESTS OFF)
      set(GLFW_BUILD_DOCS OFF)
      set(GLFW_INSTALL OFF)
      _mc_hash_arg(hash "${MC_GLFW_SHA256}")
      FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/${MC_GLFW_VERSION}.tar.gz
        ${hash})
      FetchContent_MakeAvailable(glfw)
    endfunction()
    _mc_fetch_glfw()
    target_link_libraries(mc_glfw INTERFACE glfw)
    message(STATUS "blockclonia: GLFW ${MC_GLFW_VERSION} (built from source)")
  endif()
endif()
