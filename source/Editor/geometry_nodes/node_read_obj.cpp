#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>

#include <OpenMesh/Core/IO/MeshIO.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "GCore/Components/MeshComponent.h"
#include "GCore/read_geom.h"
#include "igl/readOBJ.h"
#include "nodes/core/def/node_def.hpp"
#include "pxr/base/arch/fileSystem.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(read_obj_std)
{
    b.add_input<std::string>("Path").default_val("Default");
    b.add_output<std::vector<std::vector<float>>>("Vertices");
    b.add_output<std::vector<std::vector<float>>>("Texture Coordinates");
    b.add_output<std::vector<std::vector<float>>>("Normals");
    b.add_output<std::vector<std::vector<int>>>("Faces");
    b.add_output<std::vector<std::vector<int>>>("Face Texture Coordinates");
    b.add_output<std::vector<std::vector<int>>>("Face Normals");
    // Function content omitted
}

NODE_EXECUTION_FUNCTION(read_obj_std)
{
    std::filesystem::path executable_path;

#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    auto path_str = params.get_input<std::string>("Path");
    std::filesystem::path abs_path;
    if (!path_str.empty()) {
        abs_path = std::filesystem::path(path_str);
    }
    else {
        std::cerr << "Path is empty." << std::endl;
        return false;
    }
    if (!abs_path.is_absolute()) {
        abs_path = executable_path / abs_path;
    }
    abs_path = abs_path.lexically_normal();
    std::vector<std::vector<float>> V;
    std::vector<std::vector<float>> TC;
    std::vector<std::vector<float>> N;
    std::vector<std::vector<int>> F;
    std::vector<std::vector<int>> FTC;
    std::vector<std::vector<int>> FN;
    // Function content omitted
    auto success = igl::readOBJ(abs_path.string(), V, TC, N, F, FTC, FN);

    if (success) {
        params.set_output("Vertices", std::move(V));
        params.set_output("Texture Coordinates", std::move(TC));
        params.set_output("Normals", std::move(N));
        params.set_output("Faces", std::move(F));
        params.set_output("Face Texture Coordinates", std::move(FTC));
        params.set_output("Face Normals", std::move(FN));
        return true;
    }
    else {
        return false;
    }
}

NODE_DECLARATION_UI(read_obj_std);

NODE_DECLARATION_FUNCTION(read_obj_eigen)
{
    b.add_input<std::string>("Path").default_val("Default");
    b.add_output<Eigen::MatrixXf>("Vertices");
    b.add_output<Eigen::MatrixXf>("Texture Coordinates");
    b.add_output<Eigen::MatrixXf>("Normals");
    b.add_output<Eigen::MatrixXi>("Faces");
    b.add_output<Eigen::MatrixXi>("Face Texture Coordinates");
    b.add_output<Eigen::MatrixXi>("Face Normals");
    // Function content omitted
}

NODE_EXECUTION_FUNCTION(read_obj_eigen)
{
    std::filesystem::path executable_path;

#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    auto path_str = params.get_input<std::string>("Path");
    std::filesystem::path abs_path;
    if (!path_str.empty()) {
        abs_path = std::filesystem::path(path_str);
    }
    else {
        std::cerr << "Path is empty." << std::endl;
        return false;
    }
    if (!abs_path.is_absolute()) {
        abs_path = executable_path / abs_path;
    }
    abs_path = abs_path.lexically_normal();
    Eigen::MatrixXf V;
    Eigen::MatrixXf TC;
    Eigen::MatrixXf N;
    Eigen::MatrixXi F;
    Eigen::MatrixXi FTC;
    Eigen::MatrixXi FN;
    // Function content omitted
    auto success = igl::readOBJ(abs_path.string(), V, TC, N, F, FTC, FN);

    if (success) {
        params.set_output("Vertices", std::move(V));
        params.set_output("Texture Coordinates", std::move(TC));
        params.set_output("Normals", std::move(N));
        params.set_output("Faces", std::move(F));
        params.set_output("Face Texture Coordinates", std::move(FTC));
        params.set_output("Face Normals", std::move(FN));
        return true;
    }
    else {
        return false;
    }
}

NODE_DECLARATION_UI(read_obj_eigen);

NODE_DECLARATION_FUNCTION(read_obj_pxr)
{
    b.add_input<std::string>("Path").default_val("Default");
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(read_obj_pxr)
{
    auto path_str = params.get_input<std::string>("Path");

    try {
        Geometry geometry = read_obj_geometry(path_str);
        params.set_output("Geometry", geometry);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to read OBJ: " << e.what() << std::endl;
        return false;
    }
}

NODE_DECLARATION_UI(read_obj_pxr);

NODE_DEF_CLOSE_SCOPE
