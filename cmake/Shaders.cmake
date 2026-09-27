# GLSL -> SPIR-V, embedded as C array initialisers (*.inc) that renderer.c
# #includes. glslc always runs on the build machine, also when
# cross-compiling (toolchain files set CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER).
#
#   mc_compile_shaders(<target> SOURCE_DIR <dir> SOURCES <file>...)
#
# glslc writes a depfile per shader, so editing an #included file rebuilds
# exactly the shaders that include it, and a new #include needs no CMake edit.

set(MC_GLSLC "" CACHE FILEPATH "glslc to use (default: from the Vulkan SDK or PATH)")
if(NOT MC_GLSLC)
  if(Vulkan_GLSLC_EXECUTABLE) # FindVulkan, CMake 3.19+
    set(MC_GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
  else()
    find_program(MC_GLSLC_FOUND glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    if(NOT MC_GLSLC_FOUND)
      message(FATAL_ERROR "glslc not found. Install shaderc (Debian/Ubuntu: glslc) "
                          "or the Vulkan SDK, or set MC_GLSLC.")
    endif()
    set(MC_GLSLC "${MC_GLSLC_FOUND}")
  endif()
endif()

function(mc_compile_shaders target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "SOURCE_DIR" "SOURCES")
  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${out_dir}")
  set(werror "")
  if(MC_WERROR)
    set(werror -Werror)
  endif()
  set(outputs "")
  foreach(name IN LISTS ARG_SOURCES)
    set(src "${ARG_SOURCE_DIR}/${name}")
    set(out "${out_dir}/${name}.inc")
    add_custom_command(
      OUTPUT "${out}"
      COMMAND "${MC_GLSLC}" -O --target-env=vulkan1.0 ${werror} -mfmt=c
              -I "${ARG_SOURCE_DIR}" -MD -MF "${out}.d" -o "${out}" "${src}"
      MAIN_DEPENDENCY "${src}"
      DEPFILE "${out}.d"
      COMMENT "Compiling shader ${name}"
      VERBATIM)
    list(APPEND outputs "${out}")
  endforeach()
  # Listing the outputs as sources makes the target depend on them file by
  # file; no separate custom target is needed.
  target_sources(${target} PRIVATE ${outputs})
  target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()
