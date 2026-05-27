#include <hd_RUZINO/render_global_payload.hpp>

#include "RHI/shaderCompiler.h"
#include "RHI/rhi.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

static std::string resolve_mtlx_library_path()
{
    static const std::string cached = [] {
        const char* rel = "usd/hd_RUZINO/resources/libraries";
        namespace fs = std::filesystem;

        auto exe_dir = SlangShaderCompiler::get_exe_dir();
        std::vector<fs::path> candidates = {
            exe_dir / rel,
            exe_dir.parent_path() / rel,
            fs::current_path() / rel,
        };

        auto root = SlangShaderCompiler::find_root(".");
        candidates.push_back(root / "Binaries" / "Release" / rel);
        candidates.push_back(root / "source" / "Runtime" / "renderer" / "resources" / "libraries");

        for (auto& p : candidates) {
            if (fs::exists(p / "stdlib")) {
                return fs::canonical(p).string();
            }
        }
        return std::string(rel);
    }();
    return cached;
}

RenderGlobalPayload::RenderGlobalPayload()
{
}

RenderGlobalPayload::RenderGlobalPayload(
    std::vector<Hd_RUZINO_Camera*>* cameras,
    std::vector<Hd_RUZINO_Light*>* lights,
    pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>* materials,
    nvrhi::IDevice* nvrhi_device)
    : cameras(cameras),
      lights(lights),
      materials(materials),
      nvrhi_device(nvrhi_device),
      shader_factory(&resource_allocator)
{
    shader_factory.set_search_path(
        SlangShaderCompiler::get_shader_dir(ShaderDirType::Renderer).string());
    shader_factory.add_search_path(resolve_mtlx_library_path());
    resource_allocator.device = RHI::get_device();
    resource_allocator.shader_factory = &shader_factory;
}

RenderGlobalPayload::RenderGlobalPayload(const RenderGlobalPayload& rhs)
    : cameras(rhs.cameras),
      lights(rhs.lights),
      materials(rhs.materials),
      nvrhi_device(rhs.nvrhi_device),
      shader_factory(&resource_allocator)
{
    shader_factory.set_search_path(
        SlangShaderCompiler::get_shader_dir(ShaderDirType::Renderer).string());
    shader_factory.add_search_path(resolve_mtlx_library_path());

    resource_allocator.device = nvrhi_device;
    resource_allocator.shader_factory = &shader_factory;
}

RenderGlobalPayload& RenderGlobalPayload::operator=(
    const RenderGlobalPayload& rhs)
{
    cameras = rhs.cameras;
    lights = rhs.lights;
    materials = rhs.materials;
    nvrhi_device = rhs.nvrhi_device;
    shader_factory = ShaderFactory(&resource_allocator);

    shader_factory.set_search_path(
        SlangShaderCompiler::get_shader_dir(ShaderDirType::Renderer).string());
    shader_factory.add_search_path(resolve_mtlx_library_path());

    resource_allocator.device = nvrhi_device;
    resource_allocator.shader_factory = &shader_factory;
    return *this;
}

RUZINO_NAMESPACE_CLOSE_SCOPE