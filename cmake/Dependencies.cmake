# Third-party libraries.
#
# Vulkan (headers, loader, glslc) always comes from the system or the Vulkan
# SDK. GLFW, mimalloc and SQLite come from, depending on MC_DEPS:
#   AUTO    the system (or vcpkg/Conan/Homebrew via CMAKE_PREFIX_PATH or a
#           toolchain file) if a usable version is found, else the pinned
#           release below, downloaded and built as part of this project
#   SYSTEM  the system only; never touches the network (distro packaging)
#   FETCH   always the pinned release (Windows/macOS builds, portable Linux
#           tarballs, reproducible CI)
#
# Provides the interface targets mc::mimalloc, mc::sqlite and mc::glfw.

include(FetchContent)

# Pinned releases. To pin by content as well, set the *_SHA256 variables to
# the archive's hash (sha256sum of the downloaded tarball).
set(MC_MIMALLOC_VERSION 3.5.3 CACHE STRING "mimalloc release used when fetching")
set(MC_MIMALLOC_SHA256 "" CACHE STRING "Optional SHA256 of the mimalloc archive")
set(MC_GLFW_VERSION 3.5.1 CACHE STRING "GLFW release used when fetching")
set(MC_GLFW_SHA256 "" CACHE STRING "Optional SHA256 of the GLFW archive")
set(MC_MINIAUDIO_VERSION 0.11.25 CACHE STRING "miniaudio release used when fetching")
set(MC_MINIAUDIO_SHA256 "" CACHE STRING "Optional SHA256 of the miniaudio archive")
# sqlite.org files each release under the year it came out in and publishes
# SHA3-256 hashes; this SHA256 is of the zip whose SHA3-256 matched.
set(MC_SQLITE_VERSION 3.53.4 CACHE STRING "SQLite release used when fetching")
set(MC_SQLITE_YEAR 2026 CACHE STRING "Year directory of that release on sqlite.org")
set(MC_SQLITE_SHA256 "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d"
    CACHE STRING "SHA256 of the SQLite amalgamation zip")

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

# The game targets mimalloc 3.x (the pinned release). AUTO only takes a
# system copy at least as new as MC_MIMALLOC_MIN_AUTO and otherwise builds
# the pinned release. SYSTEM still accepts 2.1+ so distributions that ship
# an older mimalloc can package the game: mem.c needs mi_option_arena_reserve
# and mi_option_arena_eager_commit, which appeared in 2.1.0 and remain in 3.x.
set(MC_MIMALLOC_MIN_AUTO 3.5 CACHE STRING "Oldest system mimalloc MC_DEPS=AUTO will use")
if(MC_DEPS STREQUAL "AUTO")
  # The minimum goes in the call: CMake loads a package's targets only when
  # its version file accepts it, so a rejected system copy leaves no
  # imported mimalloc targets behind to collide with the ones the fetched
  # release defines. mimalloc's version file also rejects other major
  # versions, which then build the pinned release too.
  find_package(mimalloc ${MC_MIMALLOC_MIN_AUTO} CONFIG QUIET)
  if(NOT mimalloc_FOUND AND mimalloc_CONSIDERED_VERSIONS)
    list(REMOVE_DUPLICATES mimalloc_CONSIDERED_VERSIONS)
    list(JOIN mimalloc_CONSIDERED_VERSIONS ", " _mi_seen)
    message(STATUS "blockclonia: system mimalloc ${_mi_seen} is not "
                   "${MC_MIMALLOC_MIN_AUTO}+; building ${MC_MIMALLOC_VERSION} instead")
  endif()
elseif(MC_DEPS STREQUAL "SYSTEM")
  # No version in the call: mimalloc's version file rejects any other major
  # version. Checked by hand instead; SYSTEM never fetches, so nothing can
  # collide with the targets this loads.
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

# ---------------------------------------------------------------- SQLite
# World saves (save_db.c), so the core needs it, game or not. 3.31 is the
# oldest release with SQLITE_OPEN_NOFOLLOW and trusted_schema, which opening
# shared world files safely relies on; Debian bookworm has 3.40.
set(MC_SQLITE_MIN 3.31 CACHE STRING "Oldest system SQLite used")
add_library(mc_sqlite INTERFACE)
add_library(mc::sqlite ALIAS mc_sqlite)
set(MC_SQLITE_SYSTEM OFF)
if(NOT MC_DEPS STREQUAL "FETCH")
  find_package(SQLite3 ${MC_SQLITE_MIN} QUIET)
endif()
if(SQLite3_FOUND)
  if(TARGET SQLite3::SQLite3) # CMake 4.1+
    target_link_libraries(mc_sqlite INTERFACE SQLite3::SQLite3)
  else()
    target_link_libraries(mc_sqlite INTERFACE SQLite::SQLite3)
  endif()
  set(MC_SQLITE_SYSTEM ON)
  message(STATUS "blockclonia: SQLite ${SQLite3_VERSION} (system)")
elseif(MC_DEPS STREQUAL "SYSTEM")
  message(FATAL_ERROR "SQLite ${MC_SQLITE_MIN}+ not found and MC_DEPS=SYSTEM. Install it "
                      "(Debian/Ubuntu: libsqlite3-dev) or use MC_DEPS=AUTO.")
else()
  # The amalgamation: one C file, built here without the game's warnings.
  string(REPLACE "." ";" _sq_v "${MC_SQLITE_VERSION}")
  list(GET _sq_v 0 _sq_major)
  list(GET _sq_v 1 _sq_minor)
  list(GET _sq_v 2 _sq_patch)
  math(EXPR _sq_id "${_sq_major} * 1000000 + ${_sq_minor} * 10000 + ${_sq_patch} * 100")
  _mc_hash_arg(hash "${MC_SQLITE_SHA256}")
  FetchContent_Declare(sqlite
    URL https://www.sqlite.org/${MC_SQLITE_YEAR}/sqlite-amalgamation-${_sq_id}.zip
    ${hash}
    SOURCE_SUBDIR no-cmake-project)
  FetchContent_MakeAvailable(sqlite)
  add_library(mc_sqlite_impl STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
  target_include_directories(mc_sqlite_impl SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
  # Nothing the game does needs extension loading, double-quoted strings,
  # shared cache or allocation statistics; leaving them out shrinks what a
  # crafted world file can reach.
  target_compile_definitions(mc_sqlite_impl PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0
                             SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_DEPRECATED SQLITE_DEFAULT_MEMSTATUS=0)
  if(NOT MSVC)
    target_compile_options(mc_sqlite_impl PRIVATE -w)
    target_link_libraries(mc_sqlite_impl PUBLIC Threads::Threads m)
  endif()
  target_link_libraries(mc_sqlite INTERFACE mc_sqlite_impl)
  message(STATUS "blockclonia: SQLite ${MC_SQLITE_VERSION} (built from source)")
endif()

# ---------------------------------------------------------------- GLFW
if(MC_BUILD_GAME)
  add_library(mc_glfw INTERFACE)
  add_library(mc::glfw ALIAS mc_glfw)

  # Same policy as mimalloc: AUTO wants the pinned 3.5 series, SYSTEM
  # accepts any 3.3+ (everything the game calls exists since 3.3).
  set(MC_GLFW_MIN_AUTO 3.5 CACHE STRING "Oldest system GLFW MC_DEPS=AUTO will use")
  if(MC_DEPS STREQUAL "AUTO")
    # The minimum goes in the call so a system GLFW that is too old never
    # defines its imported glfw target, which would collide with the target
    # of the same name that the fetched release creates.
    find_package(glfw3 ${MC_GLFW_MIN_AUTO} CONFIG QUIET)
    if(NOT glfw3_FOUND AND glfw3_CONSIDERED_VERSIONS)
      list(REMOVE_DUPLICATES glfw3_CONSIDERED_VERSIONS)
      list(JOIN glfw3_CONSIDERED_VERSIONS ", " _glfw_seen)
      message(STATUS "blockclonia: system GLFW ${_glfw_seen} is older than "
                     "${MC_GLFW_MIN_AUTO}; building ${MC_GLFW_VERSION} instead")
    endif()
  elseif(MC_DEPS STREQUAL "SYSTEM")
    find_package(glfw3 3.3 CONFIG QUIET)
  endif()

  if(glfw3_FOUND)
    target_link_libraries(mc_glfw INTERFACE glfw)
    message(STATUS "blockclonia: GLFW ${glfw3_VERSION} (system)")
  elseif(MC_DEPS STREQUAL "SYSTEM")
    message(FATAL_ERROR "GLFW 3.3+ not found and MC_DEPS=SYSTEM. Install it "
                        "(Debian/Ubuntu: libglfw3-dev) or use MC_DEPS=AUTO.")
  else()
    # On Linux, GLFW 3.4+ builds both X11 and Wayland back ends and picks one
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

# ---------------------------------------------------------------- miniaudio
# Sound output only: the game mixes its own procedural sounds and hands
# miniaudio a callback. miniaudio loads the platform's audio library at run
# time (ALSA, PulseAudio, PipeWire through Pulse, WASAPI, CoreAudio, AAudio),
# so nothing new is linked. Its implementation is compiled once, trimmed to
# the device API, in a target of its own without the game's warnings.
# Provides mc::miniaudio and sets MC_SOUND_ENABLED.
set(MC_SOUND_ENABLED OFF)
if(MC_BUILD_GAME AND MC_SOUND)
  set(_ma_dir "")
  if(NOT MC_DEPS STREQUAL "FETCH")
    find_path(MC_MINIAUDIO_INCLUDE miniaudio.h PATH_SUFFIXES miniaudio)
    if(MC_MINIAUDIO_INCLUDE)
      set(_ma_dir "${MC_MINIAUDIO_INCLUDE}")
      message(STATUS "blockclonia: miniaudio (system, ${_ma_dir})")
    endif()
  endif()
  if(NOT _ma_dir AND NOT MC_DEPS STREQUAL "SYSTEM")
    _mc_hash_arg(hash "${MC_MINIAUDIO_SHA256}")
    # Header only: SOURCE_SUBDIR names a directory without a CMakeLists.txt,
    # so the sources are fetched and its own CMake project is never added.
    FetchContent_Declare(miniaudio
      URL https://github.com/mackron/miniaudio/archive/refs/tags/${MC_MINIAUDIO_VERSION}.tar.gz
      ${hash}
      SOURCE_SUBDIR no-cmake-project)
    FetchContent_MakeAvailable(miniaudio)
    set(_ma_dir "${miniaudio_SOURCE_DIR}")
    message(STATUS "blockclonia: miniaudio ${MC_MINIAUDIO_VERSION} (fetched)")
  endif()
  if(_ma_dir)
    set(_ma_impl "${CMAKE_CURRENT_BINARY_DIR}/miniaudio_impl.c")
    file(WRITE "${_ma_impl}.in"
"/* Generated: miniaudio's implementation, device playback only. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MINIAUDIO_IMPLEMENTATION
#include \"miniaudio.h\"
")
    configure_file("${_ma_impl}.in" "${_ma_impl}" COPYONLY)
    add_library(mc_miniaudio_impl STATIC "${_ma_impl}")
    target_include_directories(mc_miniaudio_impl SYSTEM PUBLIC "${_ma_dir}")
    if(NOT MSVC)
      target_compile_options(mc_miniaudio_impl PRIVATE -w)
      target_link_libraries(mc_miniaudio_impl PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
    endif()
    target_compile_definitions(mc_miniaudio_impl PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
                               MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
    add_library(mc_miniaudio INTERFACE)
    add_library(mc::miniaudio ALIAS mc_miniaudio)
    target_link_libraries(mc_miniaudio INTERFACE mc_miniaudio_impl)
    set(MC_SOUND_ENABLED ON)
  else()
    message(STATUS "blockclonia: miniaudio not found and MC_DEPS=SYSTEM: building without sound")
  endif()
endif()
