# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Runtime/renderer
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Runtime/renderer
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[device_object_pool_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/device_object_pool_test")
set_tests_properties([=[device_object_pool_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;10;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;0;")
add_test([=[mtlx_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/mtlx_test")
set_tests_properties([=[mtlx_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;10;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;0;")
add_test([=[renderer_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/renderer_test")
set_tests_properties([=[renderer_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;10;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Runtime/renderer/CMakeLists.txt;0;")
subdirs("nodes/lpm")
