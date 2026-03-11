#include <hd_RUZINO/render_global_payload.hpp>

#include "../../../Core/RHI/source/shaderCompiler.h"
#include "RHI/rhi.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE
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
    shader_factory.add_search_path("usd/hd_RUZINO/resources/libraries");
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
    shader_factory.add_search_path("usd/hd_RUZINO/resources/libraries");

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
    shader_factory.add_search_path("usd/hd_RUZINO/resources/libraries");

    resource_allocator.device = nvrhi_device;
    resource_allocator.shader_factory = &shader_factory;
    return *this;
}

RUZINO_NAMESPACE_CLOSE_SCOPE