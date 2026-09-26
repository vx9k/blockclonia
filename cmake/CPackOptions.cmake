# Per-generator CPack settings, read by cpack once per generator
# (CPACK_PROJECT_CONFIG_FILE in Packaging.cmake).
if(CPACK_GENERATOR MATCHES "^(TGZ|TXZ|ZIP)$")
  set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON) # archives unpack into one folder
endif()
