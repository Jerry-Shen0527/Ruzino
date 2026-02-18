// #define __GNUC__
#ifdef GEOM_USD_EXTENSION

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <filesystem>
#include <memory>

#include "GCore/Components/MaterialComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/usd_extension.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

struct ReadUsdCache {
    static constexpr bool has_storage = false;
    Ruzino::Geometry read_geometry;
    std::string file_name;
    std::string prim_path;

    float time_code = 0;
};

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(read_usd)
{
    b.add_input<std::string>("File Name").default_val("Default");
    b.add_input<std::string>("Prim Path").default_val("geometry");
    b.add_input<float>("Time Code").default_val(0).min(0).max(240);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(read_usd)
{
    auto file_name = params.get_input<std::string>("File Name");
    auto prim_path = params.get_input<std::string>("Prim Path");
    auto t = params.get_input<float>("Time Code");

    auto& cache = params.get_storage<ReadUsdCache&>();

    if (file_name == cache.file_name && prim_path == cache.prim_path &&
        t == cache.time_code) {
        params.set_output("Geometry", cache.read_geometry);
        return true;
    }

    pxr::UsdTimeCode time = pxr::UsdTimeCode(t);
    if (t == 0) {
        time = pxr::UsdTimeCode::Default();
    }

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

    std::filesystem::path abs_path;
    if (!file_name.empty()) {
        abs_path = std::filesystem::path(file_name);
    }
    else {
        spdlog::error("Path is empty.");
        return false;
    }
    if (!abs_path.is_absolute()) {
        abs_path = executable_path / abs_path;
    }
    abs_path = abs_path.lexically_normal();

    auto stage = pxr::UsdStage::Open(abs_path.string().c_str());

    if (!stage) {
        spdlog::error("Failed to open USD stage: {}", abs_path.string());
        return false;
    }

    auto sdf_path = pxr::SdfPath(prim_path.c_str());
    auto prim = stage->GetPrimAtPath(sdf_path);

    if (!prim) {
        spdlog::warn("Unable to read the prim.");
        return false;
    }

    Geometry geometry;

    // Use the shared read_geometry_from_usd function
    if (!read_geometry_from_usd(geometry, prim, time)) {
        spdlog::error("Failed to read geometry from USD");
        return false;
    }

    cache.file_name = file_name;
    cache.prim_path = prim_path;
    cache.time_code = t;
    cache.read_geometry = geometry;

    params.set_output("Geometry", geometry);
    return true;
}

NODE_DECLARATION_UI(read_usd);
NODE_DEF_CLOSE_SCOPE

#endif
