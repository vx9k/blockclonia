# Install rules and CPack.
#
#   cmake --install build --component game --strip --prefix /opt/blockclonia
#   cpack --config build/CPackConfig.cmake        (or: cmake --build build --target package)
#
# Everything we install is in the "game" component so that install rules of
# fetched dependencies (mimalloc installs its headers and libraries) stay out
# of our packages.
include(GNUInstallDirs)

# Windows zips are flat: unpack and run blockclonia.exe, with the DLLs it
# needs beside it. vulkan-1.dll is not among them; the GPU driver installs it.
if(WIN32)
  set(_bindir .)
  set(_docdir .)
else()
  set(_bindir ${CMAKE_INSTALL_BINDIR})
  set(_docdir ${CMAKE_INSTALL_DOCDIR})
endif()

install(TARGETS blockclonia RUNTIME DESTINATION ${_bindir} COMPONENT game)
install(FILES README.md DESTINATION ${_docdir} COMPONENT game)
if(MC_MIMALLOC_DLL)
  install(FILES ${MC_MIMALLOC_DLL} DESTINATION ${_bindir} COMPONENT game)
endif()
if(MSVC)
  # The Visual C++ runtime DLLs, so the zip runs without the redistributable.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${_bindir})
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT game)
  include(InstallRequiredSystemLibraries)
endif()

set(CPACK_PACKAGE_NAME blockclonia)
set(CPACK_PACKAGE_VENDOR "blockclonia")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT "blockclonia developers" CACHE STRING "Package maintainer (Debian: 'Name <email>')")
set(CPACK_STRIP_FILES ON)
set(CPACK_COMPONENTS_ALL game)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackOptions.cmake")
set(CPACK_DEB_COMPONENT_INSTALL ON)

# The package file names the target, not the build machine.
if(MC_ARM64)
  set(_arch arm64)
elseif(MC_ARM32)
  set(_arch armhf)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_arch amd64)
else()
  set(_arch i386)
endif()
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${_arch}")

if(WIN32)
  set(CPACK_GENERATOR ZIP)
elseif(APPLE)
  set(CPACK_GENERATOR TGZ)
else()
  set(CPACK_GENERATOR TGZ DEB)
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${_arch})
  set(CPACK_DEBIAN_PACKAGE_SECTION games)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  find_program(MC_DPKG_SHLIBDEPS dpkg-shlibdeps)
  if(MC_DPKG_SHLIBDEPS AND NOT CMAKE_CROSSCOMPILING)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # exact Depends: from the binary
  else()
    # Cross builds: dpkg-shlibdeps cannot inspect foreign binaries.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libvulkan1")
    if(mimalloc_FOUND) # system (shared) mimalloc; a fetched one is linked in
      if(mimalloc_VERSION VERSION_LESS 3.0)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libmimalloc2.0")
      else()
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libmimalloc3")
      endif()
    endif()
    if(glfw3_FOUND)
      string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libglfw3")
    endif()
    if(MC_SQLITE_SYSTEM)
      string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libsqlite3-0")
    endif()
  endif()
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mesa-vulkan-drivers | vulkan-icd")
endif()

include(CPack)
