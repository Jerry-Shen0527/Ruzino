RUZINO_ADD_LIB(
    stage_listener
    SHARED
    PUBLIC_LIBS usd_wrapped
    PRIVATE_LIBS spdlog::spdlog
    COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1
)
