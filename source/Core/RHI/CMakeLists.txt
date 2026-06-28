if(SLANG_FOUND)
    RUZINO_ADD_LIB(
        RHI
        SHARED
        WITH_CUDA
        PUBLIC_LIBS nvrhi SLANG::slang nodes_core
        PRIVATE_LIBS spdlog::spdlog glfw glm::glm
        PYTHON_WRAP_DIR python
        COMPILE_DEFS
        NOMINMAX
        RUZINO_BACKEND_NVRHI
    )

    if(WIN32)
    else()
        target_link_libraries(RHI PRIVATE Vulkan::Headers)
        target_compile_definitions(RHI PRIVATE VULKAN_HPP_STORAGE_SHARED)
        target_compile_definitions(RHI PRIVATE VULKAN_HPP_STORAGE_SHARED_IMPORT)
    endif()

    target_compile_definitions(RHI PRIVATE RELATIVE_TO_PROJROOT="${RELATIVE_TO_PROJROOT}")

    get_target_property(_slang_includes SLANG::slang INTERFACE_INCLUDE_DIRECTORIES)
    add_custom_command(TARGET RHI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${OUT_BINARY_DIR}/rti/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${_slang_includes} "${OUT_BINARY_DIR}/rti/slang"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${RUZINO_ROOT_DIR}/source/Runtime/renderer/resources/nvapi"
            "${OUT_BINARY_DIR}/rti/nvapi"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${RUZINO_ROOT_DIR}/source/Core/RHI/include/RHI/internal/optix"
            "${OUT_BINARY_DIR}/rti/optix"
    )

    if(WIN32)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_DX12=1)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_VULKAN=0)
        target_link_libraries(RHI PUBLIC d3d12 dxgi)
        # opengl32 provides wglCreateContext/wglMakeCurrent/wglGetProcAddress,
        # used by RHI::ensure_gl_driver_loaded to build a modern GL context for
        # headless Hydra (HgiGL) rendering.
        target_link_libraries(RHI PRIVATE opengl32)
    else()
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_DX12=0)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_VULKAN=1)
    endif()

    if(TARGET usd_ms)
        target_link_libraries(RHI PUBLIC usd_ms)
    endif()

    if(RUZINO_WITH_OPENUSD)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_OPENUSD=1)
    else()
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_OPENUSD=0)
    endif()

    if(RUZINO_WITH_CUDA)
        include(nvrtc.cmake)
    endif()

    # Configure cuda_eigen_test if it exists
    if(TARGET cuda_eigen_test)

        # Remove the /wd5054 flag that Eigen injects into cuda_eigen_test
        if(MSVC)
            get_target_property(_cuda_opts eigen INTERFACE_COMPILE_OPTIONS)
            list(REMOVE_ITEM _cuda_opts "/wd5054")
            set_target_properties(eigen PROPERTIES
                INTERFACE_COMPILE_OPTIONS "${_cuda_opts}"
            )
        endif()

        # Now link Eigen privately to avoid propagating its flags
        target_link_libraries(cuda_eigen_test PRIVATE Eigen3::Eigen CUDA::cublas CUDA::cusparse)
    endif()

    # Install additional public headers
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/RHI/shaderCompiler.h
        DESTINATION include/RHI
    )
endif()
