RUZINO_ADD_LIB(
	TerrainGen
	SHARED
	SKIP_DIRS geometry_nodes
)

target_link_libraries(TerrainGen PUBLIC glm::glm)

target_include_directories(TerrainGen PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_subdirectory(geometry_nodes)

# CPU-only gtest for the adaptive-mesh stitch contract (watertightness
# over all stitch combinations). AdaMesh.hpp is header-only pure math, so
# the exe needs nothing but the include dir.
if(TARGET gtest)
	add_executable(terrain_ada_stitch_test tests/terrain_ada_stitch_test.cpp)
	target_link_libraries(terrain_ada_stitch_test PRIVATE gtest gtest_main)
	target_include_directories(terrain_ada_stitch_test PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/include
	)
	set_target_properties(terrain_ada_stitch_test PROPERTIES ${OUTPUT_DIR} FOLDER tests)
endif()
