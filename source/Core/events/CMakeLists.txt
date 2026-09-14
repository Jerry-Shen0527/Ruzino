# Header-only broadcast event bus (extracted from GUI's WindowEventSystem).
add_library(ruzino_events INTERFACE)
target_include_directories(ruzino_events INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
