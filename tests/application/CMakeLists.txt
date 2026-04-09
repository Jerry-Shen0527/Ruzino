file(GLOB test_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(source ${test_sources})
    UCG_ADD_APP(
        SRC ${source}
        LIBS usdview_widget nodes_system GUI usd_ms nodes_ui_imgui geometry MCore stb_image
    )
endforeach()

# Only add hd_RUZINO dependencies when not in homework mode
if(NOT USTC_HOMEWORK_PLUGINS)
add_dependencies(headless_render hd_RUZINO)
add_dependencies(headless_render render_nodes)
endif()

if(pxr_FOUND)
    add_dependencies(Ruzino geometry_nodes)
endif()
add_dependencies(Ruzino basic_nodes)
# add_dependencies(Ruzino optimization)
# add back the renderer

# add_dependencies(Ruzino gl_based_render_nodes)
if(pxr_FOUND)
    add_dependencies(Ruzino RZFemBemNodes)
endif()
target_link_libraries(Ruzino PUBLIC rzpython)
target_link_libraries(rz_simulate PUBLIC rzpython)
target_link_libraries(headless_render PUBLIC rzpython)
target_link_libraries(py_importer PUBLIC rzpython)


if(RUZINO_WITH_POLYSCOPE)
    target_link_libraries(RUZINO_polyscope PUBLIC polyscope_widget)

    add_dependencies(RUZINO_polyscope geometry_nodes)
    add_dependencies(RUZINO_polyscope basic_nodes)
    add_dependencies(RUZINO_polyscope polyscope_nodes)
    add_dependencies(RUZINO_polyscope optimization)

    if(ENABLE_SUBMISSION)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../submissions_inner)
            message(STATUS "[Inner submissions found]")

            add_dependencies(RUZINO_polyscope submissions_inner_nodes)
            add_dependencies(Ruzino submissions_inner_nodes)
        endif()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../submissions)
            message(STATUS "[Outer submissions found]")
            add_dependencies(RUZINO_polyscope submissions_nodes)
            add_dependencies(Ruzino submissions_nodes)
        endif()
    endif()
endif(RUZINO_WITH_POLYSCOPE)
