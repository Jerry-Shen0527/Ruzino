

if(RUZINO_WITH_CUDA)
    set(CMAKE_CUDA_ARCHITECTURES 86)
endif()

function(Set_CUDA_Properties lib_name)
    # Set the cuda compiler to be nvcc 12.6
    find_package(CUDAToolkit REQUIRED)
    find_package(CCCL REQUIRED)
    add_compile_definitions(RUZINO_WITH_CUDA=1)
    set(CMAKE_CUDA_STANDARD 20)

    set_target_properties(${lib_name}
        PROPERTIES
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_SEPARABLE_COMPILATION ON
    )

    target_compile_features(${lib_name} PUBLIC cuda_std_20)
    target_compile_options(${lib_name}
        PUBLIC
        "$<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr;--extended-lambda;--forward-unknown-to-host-compiler;-g;-lineinfo;-rdc=true;-c;-diag-suppress=177;-diag-suppress=1394;-diag-suppress=1388;-diag-suppress=27;>"
    )

    target_link_libraries(
        ${lib_name}
        PUBLIC
        CUDA::cudart
        CUDA::nvrtc
        CCCL::CCCL
    )
endfunction()

function(UCG_ADD_TEST)
    set(options SHARED)
    set(oneValueArgs SRC)
    set(multiValueArgs LIBS LIB_FLAGS EXTRA_FILES INCLUDE_DIRS)
    cmake_parse_arguments(UCG_TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp|\.cu)" "\\2" test_name ${UCG_TEST_SRC})

    add_executable(${test_name}_test ${UCG_TEST_SRC})

    set_target_properties(${test_name}_test PROPERTIES ${OUTPUT_DIR})

    get_target_property(gtest_include GTest::gtest_main INTERFACE_INCLUDE_DIRECTORIES)

    # There should be googletest available
    target_link_libraries(${test_name}_test PUBLIC gtest gtest_main)
    target_link_libraries(${test_name}_test PUBLIC ${UCG_TEST_LIBS})
    target_include_directories(${test_name}_test PUBLIC ${UCG_TEST_INCLUDE_DIRS})
    target_compile_definitions(${test_name}_test PUBLIC NOMINMAX=1)

    # Check if the test source is a CUDA file and set CUDA properties if so
    if(RUZINO_WITH_CUDA AND UCG_TEST_SRC MATCHES "\\.cu$")
        Set_CUDA_Properties(${test_name}_test)
    endif()

    add_test(
        NAME
        ${test_name}_test
        COMMAND
        ${test_name}_test
    )
endfunction(UCG_ADD_TEST)

function(UCG_ADD_APP)
    set(options SHARED)
    set(oneValueArgs SRC)
    set(multiValueArgs LIBS LIB_FLAGS EXTRA_FILES INCLUDE_DIRS)
    cmake_parse_arguments(UCG_APP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp|\.cu)" "\\2" app_name ${UCG_APP_SRC})

    add_executable(${app_name} ${UCG_APP_SRC})

    set_target_properties(${app_name} PROPERTIES ${OUTPUT_DIR})

    target_link_libraries(${app_name} PUBLIC ${UCG_APP_LIBS})
    target_include_directories(${app_name} PUBLIC ${UCG_APP_INCLUDE_DIRS})
    target_compile_definitions(${app_name} PUBLIC NOMINMAX=1)

    # Check if the app source is a CUDA file and set CUDA properties if so
    if(RUZINO_WITH_CUDA AND UCG_APP_SRC MATCHES "\\.cu$")
        Set_CUDA_Properties(${app_name})
    endif()
endfunction(UCG_ADD_APP)
function(RUZINO_ADD_LIB LIB_NAME)
    set(options SHARED WITH_CUDA)
    set(oneValueArgs RESOURCE_COPY_TARGET)
    set(multiValueArgs LIB_FLAGS EXTRA_FILES INC_DIR PUBLIC_LIBS PRIVATE_LIBS COMPILE_OPTIONS COMPILE_DEFS USD_RESOURCE_DIRS USD_RESOURCE_FILES SKIP_DIRS PYTHON_WRAP_SRC PYTHON_WRAP_DIR)
    cmake_parse_arguments(RUZINO_ADD_LIB "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT LIB_NAME)
        get_filename_component(LIB_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
    endif()

    set(name ${LIB_NAME})

    set(folder ${CMAKE_CURRENT_SOURCE_DIR})

    file(GLOB_RECURSE ${name}_src_headers ${folder}/*.h)
    file(GLOB_RECURSE ${name}_cpp_sources ${folder}/*.cpp)

    # Exclude files under ${SRC_DIR}/test, ${USD_RESOURCE_DIRS}, and ${SKIP_DIRS}
    list(FILTER ${name}_src_headers EXCLUDE REGEX "${folder}/tests/.*")
    list(FILTER ${name}_cpp_sources EXCLUDE REGEX "${folder}/tests/.*")

    foreach(resource_dir ${RUZINO_ADD_LIB_USD_RESOURCE_DIRS})
        list(FILTER ${name}_src_headers EXCLUDE REGEX "${resource_dir}/.*")
        list(FILTER ${name}_cpp_sources EXCLUDE REGEX "${resource_dir}/.*")
    endforeach()

    if(RUZINO_ADD_LIB_PYTHON_WRAP_DIR)
        file(GLOB_RECURSE DIRED_PYTHON_WRAP_SRC ${RUZINO_ADD_LIB_PYTHON_WRAP_DIR}/*.cpp)
        list(APPEND RUZINO_ADD_LIB_PYTHON_WRAP_SRC ${DIRED_PYTHON_WRAP_SRC})
    endif()

    if(RUZINO_ADD_LIB_PYTHON_WRAP_SRC)
        list(FILTER RUZINO_ADD_LIB_PYTHON_WRAP_SRC EXCLUDE REGEX "${folder}/tests/.*")

        foreach(skip_dir ${RUZINO_ADD_LIB_SKIP_DIRS})
            list(FILTER RUZINO_ADD_LIB_PYTHON_WRAP_SRC EXCLUDE REGEX "${skip_dir}/.*")
        endforeach()
    endif()

    if(RUZINO_ADD_LIB_PYTHON_WRAP_DIR)
        list(APPEND RUZINO_ADD_LIB_SKIP_DIRS ${RUZINO_ADD_LIB_PYTHON_WRAP_DIR})
    endif()

    foreach(skip_dir ${RUZINO_ADD_LIB_SKIP_DIRS})
        list(FILTER ${name}_src_headers EXCLUDE REGEX "${skip_dir}/.*")
        list(FILTER ${name}_cpp_sources EXCLUDE REGEX "${skip_dir}/.*")
    endforeach()

    set(${name}_sources
        ${${name}_cpp_sources}
        ${${name}_headers}
        ${${name}_src_headers}
        ${RUZINO_ADD_LIB_EXTRA_FILES}
    )

    if(RUZINO_ADD_LIB_WITH_CUDA)
        if(RUZINO_WITH_CUDA)
            file(GLOB_RECURSE ${name}_cuda_sources ${folder}/*.cu ${folder}/*.cuh)
            list(FILTER ${name}_cuda_sources EXCLUDE REGEX "${folder}/tests/.*")

            foreach(resource_dir ${RUZINO_ADD_LIB_USD_RESOURCE_DIRS})
                list(FILTER ${name}_cuda_sources EXCLUDE REGEX "${resource_dir}/.*")
            endforeach()

            foreach(skip_dir ${RUZINO_ADD_LIB_SKIP_DIRS})
                list(FILTER ${name}_cuda_sources EXCLUDE REGEX "${skip_dir}/.*")
            endforeach()

            set(${name}_sources ${${name}_sources} ${${name}_cuda_sources})
            list(LENGTH ${name}_cuda_sources cuda_file_count)
        endif()
    endif()

    if(${RUZINO_ADD_LIB_SHARED})
        add_library(${name} SHARED ${RUZINO_ADD_LIB_LIB_FLAGS} ${${name}_sources})
    else()
        add_library(${name} STATIC ${RUZINO_ADD_LIB_LIB_FLAGS} ${${name}_sources})
    endif()

    # Set folder for the main library
    set_target_properties(${name} PROPERTIES FOLDER "Libraries/${name}")

    target_compile_features(${name} PUBLIC cxx_std_20)

    target_include_directories(
        ${name}
        PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
        ${RUZINO_ADD_LIB_INC_DIR}
    )

    if(RUZINO_ADD_LIB_WITH_CUDA)
        if(RUZINO_WITH_CUDA)
            Set_CUDA_Properties(${name})
        endif()
    endif()

    target_compile_options(${name} PRIVATE ${RUZINO_ADD_LIB_COMPILE_OPTIONS})
    target_compile_definitions(${name} PRIVATE ${RUZINO_ADD_LIB_COMPILE_DEFS})
    string(TOUPPER ${LIB_NAME} LIB_NAME_UPPER)
    target_compile_definitions(${name} PRIVATE BUILD_${LIB_NAME_UPPER}_MODULE=1)

    target_link_libraries(${name}
        PUBLIC ${RUZINO_ADD_LIB_PUBLIC_LIBS}
        PRIVATE ${RUZINO_ADD_LIB_PRIVATE_LIBS}
    )
    set_target_properties(${name} PROPERTIES ${OUTPUT_DIR})

    if(RUZINO_ADD_LIB_PYTHON_WRAP_SRC)
        nanobind_add_module(${name}_py NB_SHARED ${RUZINO_ADD_LIB_PYTHON_WRAP_SRC})
        set_target_properties(nanobind PROPERTIES ${OUTPUT_DIR})
        target_link_libraries(${name}_py PRIVATE ${name})

        # Set folder for Python wrapper
        set_target_properties(${name}_py PROPERTIES FOLDER "Libraries/${name}")

        # Apply the same compile definitions to Python bindings
        target_compile_definitions(${name}_py PRIVATE ${RUZINO_ADD_LIB_COMPILE_DEFS})

        target_link_libraries(${name}_py PRIVATE Python3::Python)
        target_link_libraries(nanobind PRIVATE Python3::Python)
        if(Python3_LIBRARY MATCHES "_d.lib$")
            target_compile_definitions(${name}_py PRIVATE Py_DEBUG)
        endif()

        target_link_directories(${name}_py PRIVATE ${Python3_LIBRARY_DIRS})

        set_target_properties(${name}_py PROPERTIES ${OUTPUT_DIR})

        # 仅当Python_EXECUTABLE为空时才设置Python_EXECUTABLE
        if(NOT Python_EXECUTABLE)
            set(Python_EXECUTABLE ${Python3_EXECUTABLE})
        endif()

        nanobind_add_stub(
            ${name}_py_stub
            MODULE ${name}_py
            OUTPUT ${OUT_BINARY_DIR}/${name}_py.pyi
            PYTHON_PATH ${OUT_BINARY_DIR}
            DEPENDS ${name}_py
        )
        # Set folder for Python stub
        set_target_properties(${name}_py_stub PROPERTIES FOLDER "Libraries/${name}")
    endif()

    file(GLOB test_cpp_sources ${folder}/tests/*.cpp)

    if(RUZINO_ADD_LIB_WITH_CUDA)
        if(RUZINO_WITH_CUDA)
            file(GLOB test_cuda_sources ${folder}/tests/*.cu)
            set(test_sources ${test_cpp_sources} ${test_cuda_sources})
        else()
            set(test_sources ${test_cpp_sources})
        endif()
    else()
        set(test_sources ${test_cpp_sources})
    endif()

    set(test_sources ${test_cpp_sources} ${test_cuda_sources})

    foreach(skip_dir ${RUZINO_ADD_LIB_SKIP_DIRS})
        list(FILTER test_sources EXCLUDE REGEX "${skip_dir}/.*")
    endforeach()

    foreach(source ${test_sources})
        UCG_ADD_TEST(
            SRC ${source}
            LIBS
            ${name}
            ${RUZINO_ADD_LIB_PUBLIC_LIBS}
        )
        # Set folder for test targets
        string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp|\.cu)" "\\2" test_name ${source})
        set_target_properties(${test_name}_test PROPERTIES FOLDER "Libraries/${name}/Tests")
    endforeach()

    # Ensure the copy target directory exists only if RESOURCE_COPY_TARGET is specified
    if(RUZINO_ADD_LIB_RESOURCE_COPY_TARGET)
        file(MAKE_DIRECTORY ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET})
    else()
        set(RUZINO_ADD_LIB_RESOURCE_COPY_TARGET ${OUT_BINARY_DIR}/usd)
        file(MAKE_DIRECTORY ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET})
    endif()

    # Suppress C4251 and C4996 warnings for MSVC
    if(MSVC)
        target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4996>)
        if(RUZINO_ADD_LIB_PYTHON_WRAP_SRC)
            target_compile_options(${name}_py PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4996>)
        endif()
        foreach(source ${test_sources})
            string(REGEX REPLACE "(.*/)([a-zA-Z0-9_ ]+)(\.cpp|\.cu)" "\\2" test_name ${source})
            target_compile_options(${test_name}_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/wd4251> $<$<COMPILE_LANGUAGE:CXX>:/wd4996>)
        endforeach()
    endif()

    # Copy USD resource directories and files
    foreach(resource_dir ${RUZINO_ADD_LIB_USD_RESOURCE_DIRS})
        get_filename_component(absolute_resource_dir ${resource_dir} ABSOLUTE)
        add_custom_command(
            TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${absolute_resource_dir} ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET}
            COMMENT "Copying USD resource directory ${absolute_resource_dir} to ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET}"
        )
    endforeach()

    foreach(resource_file ${RUZINO_ADD_LIB_USD_RESOURCE_FILES})
        get_filename_component(absolute_resource_file ${resource_file} ABSOLUTE)
        add_custom_command(
            TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${absolute_resource_file} ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET}
            COMMENT "Copying USD resource file ${absolute_resource_file} to ${RUZINO_ADD_LIB_RESOURCE_COPY_TARGET}"
        )
    endforeach()
endfunction()
