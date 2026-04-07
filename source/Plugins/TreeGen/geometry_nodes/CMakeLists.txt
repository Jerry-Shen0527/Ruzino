add_nodes(
	TARGET_NAME TreeGen_geometry_nodes
	JSON_DIR Plugins
	DEP_LIBS TreeGen geometry usd_wrapped
	COMPILE_DEFS NOMINMAX
	EXTRA_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/source/Editor/geometry_nodes
)
