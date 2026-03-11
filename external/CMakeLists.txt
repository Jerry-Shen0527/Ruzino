set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
add_subdirectory(eigen)
set_target_properties(demos PROPERTIES FOLDER "ThirdParty/Eigen")

if(WIN32)
    target_compile_options(eigen INTERFACE /wd5054)
endif()

# glfw
add_subdirectory(glfw)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set_target_properties(glfw update_mappings PROPERTIES FOLDER "external/GLFW3")
set_target_properties(glfw update_mappings PROPERTIES ${OUTPUT_DIR})

# nvrhi (with vulkan headers)
if(WIN32)
    set(NVRHI_WITH_NVAPI ON)
    set(NVRHI_WITH_VULKAN OFF CACHE BOOL "" FORCE)
endif()

set(NVRHI_BUILD_SHARED ON CACHE INTERNAL "Override default shared build of nvrhi")
add_subdirectory(nvrhi)
set_target_properties(nvrhi PROPERTIES ${OUTPUT_DIR})

if(WIN32)
    target_link_libraries(nvrhi PUBLIC NVAPI::NVAPI agility_sdk)
endif()

# imgui
set(imgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui" CACHE STRING "")
set(imgui_file_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ImGuiFileDialog" CACHE STRING "")
file(GLOB source
    "${imgui_SOURCE_DIR}/*.h"
    "${imgui_SOURCE_DIR}/*.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.*"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.*"
    "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.*"
    "${imgui_file_SOURCE_DIR}/*.h"
    "${imgui_file_SOURCE_DIR}/*.cpp"
)
add_library(imgui SHARED ${source})
target_include_directories(imgui
    PUBLIC "${imgui_SOURCE_DIR}"
    PUBLIC "${imgui_SOURCE_DIR}/backends"
    PUBLIC "${imgui_SOURCE_DIR}/misc/cpp"
    PUBLIC "${imgui_file_SOURCE_DIR}"
)
target_include_directories(imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/user_imgui")
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="imgui_user_config.h")

# target_compile_definitions(imgui PRIVATE IMGUI_API=__declspec\(dllexport\))
if(MSVC)
    target_compile_definitions(imgui PRIVATE IMGUI_API=__declspec\(dllexport\))
else()
    target_compile_definitions(imgui PRIVATE IMGUI_API=__attribute__\(\(visibility\(\"default\"\)\)\))
    target_compile_options(imgui PRIVATE -fvisibility=hidden)
endif()

if(WIN32)
else()
    target_link_libraries(imgui PUBLIC Vulkan::Headers)
endif()

target_link_libraries(imgui PUBLIC glfw)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1)
set_target_properties(imgui PROPERTIES FOLDER "external" ${OUTPUT_DIR})

add_subdirectory(stb_image)

# autodiff
set(AUTODIFF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AUTODIFF_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(AUTODIFF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AUTODIFF_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(autodiff)
set_target_properties(autodiff PROPERTIES ${OUTPUT_DIR})

# Set all third-party targets to ThirdParty folder
set_target_properties(glfw update_mappings PROPERTIES FOLDER "ThirdParty")
set_target_properties(nvrhi PROPERTIES FOLDER "ThirdParty")
set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")

set_target_properties(autodiff PROPERTIES FOLDER "ThirdParty")
set_target_properties(stb_image PROPERTIES FOLDER "ThirdParty")
set_target_properties(uninstall PROPERTIES FOLDER "ThirdParty")


add_library(exprtk INTERFACE)
target_include_directories(exprtk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/exprtk")

set(NB_CREATE_INSTALL_RULES OFF CACHE BOOL "Disable nanobind install rules" FORCE)
add_subdirectory(nanobind)

# ========== Install external dependencies ==========
# These external libraries are built as shared libraries but may not have install rules.
# We need to install them so that the install directory is self-contained.

# Install glfw
install(TARGETS glfw
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib
)

# Install imgui
install(TARGETS imgui
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib
)
# Install imgui headers
install(DIRECTORY ${imgui_SOURCE_DIR}/
    DESTINATION include/imgui
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${imgui_SOURCE_DIR}/backends/
    DESTINATION include/imgui/backends
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${imgui_SOURCE_DIR}/misc/cpp/
    DESTINATION include/imgui/misc/cpp
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${imgui_file_SOURCE_DIR}/
    DESTINATION include/ImGuiFileDialog
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/user_imgui/
    DESTINATION include/user_imgui
    FILES_MATCHING PATTERN "*.h"
)

# Install nanobind (conditional - target may not exist)
if(TARGET nanobind)
    install(TARGETS nanobind
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
    )
endif()

# Install nvrhi
if(TARGET nvrhi)
    install(TARGETS nvrhi
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
    )
endif()

# Install autodiff
if(TARGET autodiff)
    install(TARGETS autodiff
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib
    )
endif()


# polyscope
# add_subdirectory(polyscope)
# set_target_properties(polyscope PROPERTIES ${OUTPUT_DIR})
# set_target_properties(glad PROPERTIES ${OUTPUT_DIR})
# set_target_properties(polyscope PROPERTIES FOLDER "ThirdParty")
# set_target_properties(glad PROPERTIES FOLDER "ThirdParty")