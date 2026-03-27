# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Core/rznode/system
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Core/rznode/system
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[node_system_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/node_system_test")
set_tests_properties([=[node_system_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rznode/system/CMakeLists.txt;1;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rznode/system/CMakeLists.txt;0;")
subdirs("tests")
