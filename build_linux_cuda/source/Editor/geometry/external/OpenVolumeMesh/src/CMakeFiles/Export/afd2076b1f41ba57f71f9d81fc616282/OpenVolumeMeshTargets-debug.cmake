#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenVolumeMesh::OpenVolumeMesh" for configuration "Debug"
set_property(TARGET OpenVolumeMesh::OpenVolumeMesh APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(OpenVolumeMesh::OpenVolumeMesh PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libOpenVolumeMeshd.so.3.4"
  IMPORTED_SONAME_DEBUG "libOpenVolumeMeshd.so.3.4"
  )

list(APPEND _cmake_import_check_targets OpenVolumeMesh::OpenVolumeMesh )
list(APPEND _cmake_import_check_files_for_OpenVolumeMesh::OpenVolumeMesh "${_IMPORT_PREFIX}/lib/libOpenVolumeMeshd.so.3.4" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
