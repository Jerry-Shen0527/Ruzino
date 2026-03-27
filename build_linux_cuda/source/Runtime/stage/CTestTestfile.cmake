# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Runtime/stage
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Runtime/stage
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[ecs_api_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/ecs_api_test")
set_tests_properties([=[ecs_api_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Runtime/stage/CMakeLists.txt;1;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Runtime/stage/CMakeLists.txt;0;")
add_test([=[stage_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/stage_test")
set_tests_properties([=[stage_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Runtime/stage/CMakeLists.txt;1;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Runtime/stage/CMakeLists.txt;0;")
