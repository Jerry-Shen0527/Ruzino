# CMake generated Testfile for 
# Source directory: /home/jerry/Workspace/Ruzino/source/Core/rzfembem
# Build directory: /home/jerry/Workspace/Ruzino/build_linux_cuda/source/Core/rzfembem
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[debug_gradient_inner_product_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/debug_gradient_inner_product_test")
set_tests_properties([=[debug_gradient_inner_product_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[expression_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/expression_test")
set_tests_properties([=[expression_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[expression_performance_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/expression_performance_test")
set_tests_properties([=[expression_performance_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[exprtk_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/exprtk_test")
set_tests_properties([=[exprtk_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[fem_bem_problem_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/fem_bem_problem_test")
set_tests_properties([=[fem_bem_problem_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[integration_performance_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/integration_performance_test")
set_tests_properties([=[integration_performance_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
add_test([=[numerical_integral_test]=] "/home/jerry/Workspace/Ruzino/Binaries/Debug/numerical_integral_test")
set_tests_properties([=[numerical_integral_test]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;74;add_test;/home/jerry/Workspace/Ruzino/source/Core/rznode/cmake/AddLibrary.cmake;285;UCG_ADD_TEST;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;11;RUZINO_ADD_LIB;/home/jerry/Workspace/Ruzino/source/Core/rzfembem/CMakeLists.txt;0;")
subdirs("nodes")
