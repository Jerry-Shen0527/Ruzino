# Build compile definitions list
set(GEOMETRY_NODES_COMPILE_DEFS NOMINMAX)
if(ENABLE_GEOM_USD_EXTENSION)
	list(APPEND GEOMETRY_NODES_COMPILE_DEFS GEOM_USD_EXTENSION=1)
endif()

# Add shader directory definition
list(APPEND GEOMETRY_NODES_COMPILE_DEFS GEOM_NODES_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")

add_nodes(
	TARGET_NAME geometry_nodes
	SRC_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
	DEP_LIBS stage nodes_system geometry Eigen3::Eigen autodiff igl::core igl_restricted::triangle RZFemBem usd_ms
	COMPILE_DEFS ${GEOMETRY_NODES_COMPILE_DEFS}
)

target_link_libraries(node_points_to_mesh PRIVATE usd_ms)
target_link_libraries(node_texture_io PRIVATE stb_image)

# Install geometry nodes shader files
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/SPH/shaders/
	DESTINATION bin/shaders/geom_nodes/SPH
	PATTERN ".git" EXCLUDE
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/BrushSimulation/shaders/
	DESTINATION bin/shaders/geom_nodes/BrushSimulation
	PATTERN ".git" EXCLUDE
)

add_library(tetgen STATIC ${CMAKE_CURRENT_SOURCE_DIR}/tetgen/tetgen.cxx ${CMAKE_CURRENT_SOURCE_DIR}/tetgen/tetgen.h ${CMAKE_CURRENT_SOURCE_DIR}/tetgen/predicates.cxx)
set_target_properties(tetgen PROPERTIES ${OUTPUT_DIR})
if(MSVC)
	target_compile_options(tetgen PRIVATE /wd4477)
endif()
target_link_libraries(node_tetgen PRIVATE tetgen)
target_include_directories(node_tetgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tetgen)
