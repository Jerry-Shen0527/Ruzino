RUZINO_ADD_LIB(
	stage
	SHARED
	PUBLIC_LIBS
		usd_wrapped
		EnTT::EnTT
		glm::glm
		geometry
	PYTHON_WRAP_DIR python
	PRIVATE_LIBS
		spdlog::spdlog
		nodes_system
		MaterialXCore
		MaterialXFormat
		stage_listener
	COMPILE_DEFS
		NOMINMAX=1
		RUZINO_BUILD_MODULE=1
)

# Link Boost.Python and USD Python bindings using CMake targets
# The 'python' target is provided by OpenUSD and contains usd_python with Boost.Python
# The 'boost' target provides Boost headers including boost/python.hpp
target_link_libraries(stage PRIVATE usd_wrapped)

# Link dependencies for stage_py Python wrapper module
# Needs python/boost for USD interop + geometry for GeomPayload + nodes_system
target_link_libraries(stage_py PRIVATE usd_wrapped geometry nodes_system)

# CRITICAL: Define NOMINMAX for stage_py to prevent Windows min/max macro issues
# USD's robin_growth_policy.h uses std::numeric_limits<>::max() which conflicts with Windows macros
target_compile_definitions(stage_py PRIVATE NOMINMAX)
