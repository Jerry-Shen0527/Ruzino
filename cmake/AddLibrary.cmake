
set(RZNODE_CUDA_EXTRA_FLAGS "--forward-unknown-to-host-compiler;-c" CACHE STRING "Extra CUDA compiler flags for Ruzino" FORCE)

set(RZNODE_LINK_PYTHON_TO_NANOBIND ON CACHE BOOL "Link Python3::Python to nanobind targets" FORCE)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../source/Core/rznode/cmake/AddLibrary.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../source/Core/rznode/cmake/AddLibrary.cmake")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/rznode/AddLibrary.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/rznode/AddLibrary.cmake")
else()
    message(FATAL_ERROR "Cannot find rznode/AddLibrary.cmake. Please ensure rznode submodule is initialized.")
endif()
