# CUDA library for rzsim - isolated to avoid flag propagation issues

RUZINO_ADD_LIB(
    rzsim_cuda
    SHARED
    WITH_CUDA
    PUBLIC_LIBS RHI Eigen3::Eigen RZSolver
)

target_compile_definitions(rzsim_cuda PRIVATE BUILD_RZSIM_CUDA_MODULE)
# Add test executable for Laplace matrix
if(GTEST_FOUND)
    add_executable(laplace_matrix_test ../tests/laplace_matrix.cpp)
    target_link_libraries(laplace_matrix_test PRIVATE rzsim_cuda RHI gtest gtest_main)
    target_include_directories(laplace_matrix_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    set_target_properties(laplace_matrix_test PROPERTIES FOLDER tests)
endif()