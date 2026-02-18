# Plugins CMakeLists.txt - Linux 临时禁用不兼容的插件

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/BPM)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/TerrainGen)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/TreeGen)

# Embree/GL 渲染器插件
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/hd_RUZINO_Embree)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/hd_RUZINO_GL)

# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/light_field)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/optimization)
