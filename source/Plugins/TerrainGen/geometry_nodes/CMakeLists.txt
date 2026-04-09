add_nodes(
	TARGET_NAME TerrainGen_geometry_nodes
	JSON_DIR Plugins
	DEP_LIBS TerrainGen geometry usd_ms
	COMPILE_DEFS NOMINMAX
	EXTRA_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/source/Editor/geometry_nodes
)
