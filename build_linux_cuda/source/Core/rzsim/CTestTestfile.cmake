# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Core/rzsim
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Core/rzsim
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[adjacency_map_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/adjacency_map_test")
set_tests_properties([=[adjacency_map_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;7;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;0;")
add_test([=[laplace_matrix_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/laplace_matrix_test")
set_tests_properties([=[laplace_matrix_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;7;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;0;")
add_test([=[reduced_basis_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/reduced_basis_test")
set_tests_properties([=[reduced_basis_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;7;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;0;")
add_test([=[reduced_basis_simple_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/reduced_basis_simple_test")
set_tests_properties([=[reduced_basis_simple_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;7;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzsim/CMakeLists.txt;0;")
subdirs("rzsim_cuda")
subdirs("nodes")
subdirs("renderer_nodes")
subdirs("geometry_nodes")
