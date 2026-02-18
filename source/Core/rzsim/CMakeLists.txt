# Create the isolated CUDA library first
if (RUZINO_WITH_CUDA)

add_subdirectory(rzsim_cuda)

# Main rzsim library - links to both CUDA and geometry libraries
RUZINO_ADD_LIB(
	rzsim
	SHARED
	WITH_CUDA
	PUBLIC_LIBS RHI geometry rzsim_cuda igl_core
	SKIP_DIRS nodes renderer_nodes geometry_nodes rzsim_cuda
)

# Add Spectra include directory
target_include_directories(rzsim PUBLIC ${CMAKE_SOURCE_DIR}/external/spectra/include)

add_subdirectory(nodes)
add_subdirectory(renderer_nodes)
if(pxr_FOUND)
    add_subdirectory(geometry_nodes)
endif()
# Tests
target_link_libraries(reduced_basis_simple_test PRIVATE rzsim geometry igl_core)

endif()