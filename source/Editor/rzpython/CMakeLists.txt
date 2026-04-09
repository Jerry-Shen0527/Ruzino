RUZINO_ADD_LIB(
	rzpython
	SHARED
	PUBLIC_LIBS GUI nanobind rzconsole
	PRIVATE_LIBS
		glm::glm spdlog::spdlog Python3::Module
	COMPILE_DEFS
		NOMINMAX=1
		ASIO_STANDALONE
	SKIP_DIRS
		ext
)

target_include_directories(rzpython PUBLIC ${NB_DIR}/include)
target_include_directories(rzpython PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ext/asio/include)

target_link_libraries(rzpython_runtime_test PUBLIC RHI usd_ms)
target_link_libraries(rzpython_console_test PUBLIC RHI usd_ms)
