if(RUZINO_WITH_OPENUSD)
  set(PRIVATE_LIBS spdlog::spdlog usd_ms RHI)
  if(UNIX AND NOT APPLE)
    list(APPEND PRIVATE_LIBS PkgConfig::EGL)
  elseif(WIN32)
    list(APPEND PRIVATE_LIBS OpenGL32)
  endif()
  set(COMPILE_DEFS NOMINMAX=1 RUZINO_BUILD_MODULE=1)

  RUZINO_ADD_LIB(
    usdview_widget
    SHARED
    PUBLIC_LIBS stage geometry GUI
    PRIVATE_LIBS ${PRIVATE_LIBS}
    COMPILE_DEFS ${COMPILE_DEFS}
  )
endif()
