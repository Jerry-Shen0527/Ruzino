
RUZINO_ADD_LIB(
	blueprints
	PUBLIC_LIBS GUI nodes_core RHI
	PRIVATE_LIBS glfw spdlog::spdlog stb_image nodes_system test_nodes
)

set_target_properties(blueprints PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nodes_ui_imgui PUBLIC ${CMAKE_CURRENT_LIST_DIR}/imgui/imgui-node-editor)