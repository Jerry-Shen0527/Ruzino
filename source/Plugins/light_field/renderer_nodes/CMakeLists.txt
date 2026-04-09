add_nodes(
	TARGET_NAME light_field_renderer_nodes
	JSON_DIR renderer_plugins
	DEP_LIBS geometry RHI gtest stb_image light_field hd_RUZINO shaders_interface usd_ms
	COMPILE_DEFS LIGHT_FIELD_SHADERS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
