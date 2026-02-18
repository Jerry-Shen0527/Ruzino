add_subdirectory(basic_nodes)

# add back the renderer
add_subdirectory(renderer)
if(pxr_FOUND)
    add_subdirectory(stage)
    add_subdirectory(stage_listener)
endif()
add_subdirectory(usdview_widget)
# add_subdirectory(polyscope_widget)
# add_subdirectory(polyscope_nodes)