add_nodes(
	TARGET_NAME basic_nodes
	CONVERSION_DIRS conversion/
	DEP_LIBS stage nodes_system usd_wrapped
	COMPILE_DEFS NOMINMAX
)
