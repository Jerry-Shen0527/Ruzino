# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Editor/geometry
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Editor/geometry
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[geom_algorithms_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/geom_algorithms_test")
set_tests_properties([=[geom_algorithms_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;13;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;0;")
add_test([=[geom_hash_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/geom_hash_test")
set_tests_properties([=[geom_hash_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;13;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;0;")
add_test([=[openvolumemesh_bind_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/openvolumemesh_bind_test")
set_tests_properties([=[openvolumemesh_bind_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;13;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;0;")
add_test([=[test_cow_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/test_cow_test")
set_tests_properties([=[test_cow_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;13;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;0;")
add_test([=[test_gpu_interface_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/test_gpu_interface_test")
set_tests_properties([=[test_gpu_interface_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;13;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Editor/geometry/CMakeLists.txt;0;")
subdirs("external")
