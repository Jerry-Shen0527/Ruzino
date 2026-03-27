# Install script for directory: /home/jerry/Workspace/Ruzino/source/Runtime/renderer

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/usd/hd_RUZINO/resources" TYPE DIRECTORY FILES "/home/jerry/Workspace/Ruzino/source/Runtime/renderer/resources/" REGEX "/\\.git$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/usd/hd_RUZINO/resources" TYPE DIRECTORY FILES "/home/jerry/Workspace/Ruzino/source/Runtime/renderer/nodes/shaders/" REGEX "/\\.git$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libhd_RUZINO.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libhd_RUZINO.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/hd_RUZINO_py.cpython-312-x86_64-linux-gnu.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES "/home/jerry/Workspace/Ruzino/source/Runtime/renderer/include/" REGEX "/\\.git$" EXCLUDE REGEX "/tests$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/shaders/renderer/shaders" TYPE DIRECTORY FILES "/home/jerry/Workspace/Ruzino/source/Runtime/renderer/nodes/shaders/shaders/" REGEX "/\\.git$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/shaders/gpu_assembler" TYPE DIRECTORY FILES "/home/jerry/Workspace/Ruzino/source/Runtime/renderer/source/shaders/" REGEX "/\\.git$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libaccumulate.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libaccumulate.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libautomatic_tonemapper.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libautomatic_tonemapper.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libgamma_correction.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libgamma_correction.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libmaterial_brdf_analyzer.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libmaterial_brdf_analyzer.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_lpm.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_lpm.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_node_render_ray_generation.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_node_render_ray_generation.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_blit_to_present.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_blit_to_present.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_create_empty_texture.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_create_empty_texture.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_deferred_lighting.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_deferred_lighting.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_environment_pass.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_environment_pass.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_material_eval_sample_pdf.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_material_eval_sample_pdf.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_motion_vec.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_motion_vec.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_present.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_present.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_print_debug.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_print_debug.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_rasterize.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rasterize.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_ray_launch.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ray_launch.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_rayleigh_sommerfeld.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rayleigh_sommerfeld.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_rng_buffer.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_buffer.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_rng_texture.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_rng_texture.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_scatter_contribution.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_scatter_contribution.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_shadertoy.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadertoy.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_shadow_mapping.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_shadow_mapping.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_ssao.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_ssao.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_taa.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_taa.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_render_transparent_refraction.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_render_transparent_refraction.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnode_test_socket.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnode_test_socket.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libpath_tracing.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libpath_tracing.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libray_marching.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libray_marching.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libtome_mapping.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtome_mapping.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libtranslation_orientation_to_transforms.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libtranslation_orientation_to_transforms.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libcuda_to_nvrhi.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcuda_to_nvrhi.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE MODULE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/libnvrhi_to_cuda.so")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so"
         OLD_RPATH "/home/jerry/Workspace/Ruzino/SDK/OpenUSD/Debug/lib:/home/jerry/Workspace/Ruzino/Binaries/Debug:/usr/local/cuda-13.2/targets/x86_64-linux/lib:/home/jerry/Workspace/Ruzino/SDK/slang/lib:/home/jerry/Workspace/Ruzino/build_linux_cuda/Build/lib:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libnvrhi_to_cuda.so")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE FILE FILES "/home/jerry/Workspace/Ruzino/Binaries/Debug/render_nodes.json")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/home/jerry/Workspace/Ruzino/build_linux_cuda/source/Runtime/renderer/nodes/lpm/cmake_install.cmake")
endif()

