include(FindPackageHandleStandardArgs)

# Define search paths based on user input and environment variables
set(AgilitySDK_SEARCH_DIR ${AgilitySDK_ROOT_DIR})
##################################
# Find the AgilitySDK include dir
##################################

message("Searching for Agility SDK in ${AgilitySDK_SEARCH_DIR}/build/native/include")

find_path(AgilitySDK_INCLUDE_DIRS d3d12.h
    PATHS ${AgilitySDK_SEARCH_DIR}/build/native/include)

find_package_handle_standard_args(AgilitySDK REQUIRED_VARS AgilitySDK_INCLUDE_DIRS)

##################################
# Create targets
##################################

message ("Agility SDK found. Include directory: ${AgilitySDK_INCLUDE_DIRS}")

if(NOT CMAKE_VERSION VERSION_LESS 3.0 AND AgilitySDK_FOUND)
add_library(agility_sdk INTERFACE)
set_target_properties(agility_sdk PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES  ${AgilitySDK_INCLUDE_DIRS}
          IMPORTED_IMPLIB  ${AgilitySDK_LIBRARIES}
          IMPORTED_LOCATION  ${AgilitySDK_LIBRARIES})
endif()