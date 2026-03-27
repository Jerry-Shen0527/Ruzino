#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenMeshCore" for configuration "Debug"
set_property(TARGET OpenMeshCore APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenMeshCore PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libOpenMeshCored.so.11.0"
  IMPORTED_SONAME_DEBUG "libOpenMeshCored.so.11.0"
  )

list(APPEND _cmake_import_check_targets OpenMeshCore )
list(APPEND _cmake_import_check_files_for_OpenMeshCore "${_IMPORT_PREFIX}/lib/libOpenMeshCored.so.11.0" )

# Import target "OpenMeshTools" for configuration "Debug"
set_property(TARGET OpenMeshTools APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenMeshTools PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libOpenMeshToolsd.so.11.0"
  IMPORTED_SONAME_DEBUG "libOpenMeshToolsd.so.11.0"
  )

list(APPEND _cmake_import_check_targets OpenMeshTools )
list(APPEND _cmake_import_check_files_for_OpenMeshTools "${_IMPORT_PREFIX}/lib/libOpenMeshToolsd.so.11.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
