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

    if(TARGET hgi)
        target_link_libraries(RHI PUBLIC hgi)
        target_compile_definitions(RHI PUBLIC RHI_WITH_OPENUSD_HGI=1)
    endif()

    if(WIN32)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_DX12=1)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_VULKAN=0)
        target_link_libraries(RHI PUBLIC d3d12 dxgi)
    else()
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_DX12=0)
        target_compile_definitions(RHI PUBLIC RUZINO_WITH_VULKAN=1)
    endif()

    if(TARGET garch)
        target_link_libraries(RHI PUBLIC garch)
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

    # Install additional public headers from source directory
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/source/shaderCompiler.h
        DESTINATION include/RHI
    )
endif()
