# Remove the /wd5054 flag that Eigen injects into cuda_eigen_test
if(MSVC)
    get_target_property(_cuda_opts eigen INTERFACE_COMPILE_OPTIONS)
    list(REMOVE_ITEM _cuda_opts "/wd5054")
    set_target_properties(eigen PROPERTIES
        INTERFACE_COMPILE_OPTIONS "${_cuda_opts}"
    )

endif()

RUZINO_ADD_LIB(
    RZFemBem
    SHARED
    WITH_CUDA
    PUBLIC_LIBS RZSolver exprtk geometry igl_core OpenVolumeMesh::OpenVolumeMesh
    COMPILE_DEFS NOMINMAX
    SKIP_DIRS nodes
)

target_compile_options(RZFemBem INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

add_subdirectory(nodes)
