# HosekWilkieSky codeless USD schema — resource-only plugin (no DLL).
#
# The checked-in artifacts in resources/ (plugInfo.json +
# generatedSchema.usda) ARE the source of truth for builds; regenerate them
# only when schema.usda.in changes, via regen_schema.py (needs jinja2 and
# the OpenUSD SDK source tree — see that script's docstring).
#
# Mirroring into ${OUT_BINARY_DIR}/usd/RuzinoSky/resources makes the type
# discoverable by the runtime plugin scanner: the root usd/plugInfo.json
# includes "*/resources/", exactly like the SDK's own schema resources.
set(RUZINO_SKY_SRC_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(RUZINO_SKY_BIN_RESOURCES ${OUT_BINARY_DIR}/usd/RuzinoSky/resources)

add_custom_target(ruzino_sky_schema_resources ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${RUZINO_SKY_SRC_RESOURCES}
		${RUZINO_SKY_BIN_RESOURCES}
	COMMENT "Mirroring RuzinoSky codeless schema into the runtime usd/ tree"
)

install(DIRECTORY ${RUZINO_SKY_SRC_RESOURCES}/
	DESTINATION bin/usd/RuzinoSky/resources
	PATTERN ".git" EXCLUDE
)
