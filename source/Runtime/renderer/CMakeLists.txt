add_library(shaders_interface INTERFACE)
target_include_directories(shaders_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nodes/shaders/shaders)
target_include_directories(shaders_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nodes/render_node_base)
target_include_directories(shaders_interface INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/nodes/)
target_compile_definitions(shaders_interface INTERFACE NOMINMAX)

# Skip hd_RUZINO (NVRHI backend) compilation in homework mode to avoid conflicts with GL backend
if(NOT USTC_HOMEWORK_PLUGINS)

RUZINO_ADD_LIB(
	hd_RUZINO
	SHARED
	PUBLIC_LIBS spdlog::spdlog GPUContext usd_ms MCore
	PRIVATE_LIBS RHI nodes_system shaders_interface osdCPU openvdb
	COMPILE_DEFS
		NOMINMAX RUZINO_DIRECT_VK_DISPLAY
	USD_RESOURCE_DIRS resources nodes/shaders
	SKIP_DIRS nodes
	RESOURCE_COPY_TARGET ${OUT_BINARY_DIR}/usd/hd_RUZINO/resources
	PYTHON_WRAP_DIR python
)

if(MSVC)
	target_compile_options(hd_RUZINO PRIVATE /bigobj)
endif()
target_compile_definitions(hd_RUZINO PRIVATE HOST_CODE=1 _USE_MATH_DEFINES)
target_compile_definitions(hd_RUZINO PUBLIC RUZINO_BACKEND_NVRHI)
target_compile_definitions(hd_RUZINO_py PRIVATE NOMINMAX)
target_link_libraries(hd_RUZINO_py PRIVATE shaders_interface OpenGL32)
target_include_directories(hd_RUZINO PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/nodes/shaders/shaders)
target_include_directories(hd_RUZINO PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

# Install renderer shader files
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/nodes/shaders/shaders/
	DESTINATION bin/shaders/renderer/shaders
	PATTERN ".git" EXCLUDE
)

# Install GPU assembler shader files
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/source/shaders/
	DESTINATION bin/shaders/gpu_assembler
	PATTERN ".git" EXCLUDE
)

add_nodes(
	TARGET_NAME render_nodes
	SRC_DIRS nodes
	CONVERSION_DIRS nodes/conversion
	DEP_LIBS usd_ms RHI hd_RUZINO shaders_interface
	COMPILE_DEFS NOMINMAX
	EXTRA_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/source
)

add_subdirectory(nodes/lpm)
target_link_libraries(node_lpm PUBLIC ffx_lpm)

endif() # NOT USTC_HOMEWORK_PLUGINS
