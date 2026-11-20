# FindNVAPI.cmake

# Locate NVAPI library
# This module defines
# NVAPI::NVAPI target to include NVAPI headers and libraries

set(NVAPI_INCLUDE_DIR "${NVAPI_ROOT_DIR}/include")
set(NVAPI_LIBRARY "${NVAPI_ROOT_DIR}/lib/nvapi64.lib")
if(NOT EXISTS "${NVAPI_INCLUDE_DIR}/nvapi.h")
  set(NVAPI_INCLUDE_DIR "NVAPI_INCLUDE_DIR-NOTFOUND")
endif()
if(NOT EXISTS "${NVAPI_LIBRARY}")
  set(NVAPI_LIBRARY "NVAPI_LIBRARY-NOTFOUND")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NVAPI REQUIRED_VARS NVAPI_INCLUDE_DIR NVAPI_LIBRARY)

if(NVAPI_FOUND AND NOT TARGET NVAPI::NVAPI)
  add_library(NVAPI::NVAPI INTERFACE IMPORTED)
  set_target_properties(NVAPI::NVAPI PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NVAPI_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${NVAPI_LIBRARY}"
  )
endif()
