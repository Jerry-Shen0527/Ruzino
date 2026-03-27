# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-src"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-build"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/tmp"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/src/triangle-populate-stamp"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/src"
  "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/src/triangle-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/src/triangle-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/jerry/Workspace/Ruzino/build_linux_cuda/_deps/triangle-subbuild/triangle-populate-prefix/src/triangle-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
