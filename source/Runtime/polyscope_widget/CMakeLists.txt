RUZINO_ADD_LIB(
    polyscope_widget
    SHARED
    PUBLIC_LIBS polyscope stage GUI usd_wrapped stage_listener
    PRIVATE_LIBS spdlog::spdlog RHI
    COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1
)
