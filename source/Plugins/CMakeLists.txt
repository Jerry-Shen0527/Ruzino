# Plugins CMakeLists.txt - Linux 临时禁用不兼容的插件

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/BPM)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/TerrainGen)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/TreeGen)

# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/light_field)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/optimization)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/scripts/
    DESTINATION bin/scripts
    FILES_MATCHING PATTERN "*.py")
