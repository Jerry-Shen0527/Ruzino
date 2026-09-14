add_nodes(
	TARGET_NAME TerrainGen_geometry_nodes
	JSON_DIR Plugins
	DEP_LIBS TerrainGen geometry usd_ms
	COMPILE_DEFS NOMINMAX
	EXTRA_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/source/Editor/geometry_nodes
		${CMAKE_SOURCE_DIR}/external/stb_image/stb
	# SRC_DIRS default (CMAKE_CURRENT_SOURCE_DIR): terrain family nodes
	# (terrain_heightfield / terrain_erode_hydraulic / terrain_erode_thermal /
	# terrain_texture_bake) are globbed from this directory.
)
