set(deps
		stage nodes_system usd_wrapped geometry Eigen3::Eigen autodiff igl::core igl_restricted::triangle RHI hd_RUZINO_GL
)
# If the student add a custom utility lib
message(STATUS "[Utils status: ${student_name}_${util_lib_target_name}_AVAILABLE] " ${${student_name}_${util_lib_target_name}_AVAILABLE})
if (${student_name}_${util_lib_target_name}_AVAILABLE)
	message(STATUS "[${student_name}_${util_lib_target_name}_AVAILABLE] " Available!)
	list(APPEND deps ${student_name}_${util_lib_target_name})
endif ()

add_nodes_with_prefix(
	TARGET_NAME nodes_hw_submissions_render
	CUS_PREFIX ${student_name}
	DEP_LIBS ${deps}
	COMPILE_DEFS NOMINMAX CGHW_STUDENT_NAME=${student_name} RENDER_NODES_FILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}" RUZINO_BACKEND_GL
	EXTRA_INCLUDE_DIRS ${editor_dir} ${editor_dir}/geometry_nodes ${PROJECT_SOURCE_DIR}/source/Plugins/hd_RUZINO_GL/nodes
)
