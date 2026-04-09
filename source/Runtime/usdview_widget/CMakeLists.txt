if(RUZINO_WITH_OPENUSD)
  set(PRIVATE_LIBS spdlog::spdlog usd_ms RHI PkgConfig::EGL)
  set(COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1)

  RUZINO_ADD_LIB(
    usdview_widget
    SHARED
    PUBLIC_LIBS stage geometry GUI
    PRIVATE_LIBS ${PRIVATE_LIBS}
    COMPILE_DEFS ${COMPILE_DEFS}
  )
endif()
