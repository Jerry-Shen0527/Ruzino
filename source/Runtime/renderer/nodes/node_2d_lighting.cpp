#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3f.h>

#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

struct Lighting2DStorage {
    constexpr static bool has_storage = false;

    nvrhi::TextureHandle output;
    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    ResourceAllocator* rc = nullptr;

    ~Lighting2DStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
        }
    }
};

NODE_DECLARATION_FUNCTION(lighting_2d)
{
    b.add_input<nvrhi::TextureHandle>("Normal Map");
    b.add_input<nvrhi::TextureHandle>("Base Color");
    b.add_input<pxr::GfVec3f>("Light Direction");
    b.add_input<pxr::GfVec3f>("Light Color");
    b.add_input<float>("Ambient").default_val(0.2f).min(0.0f).max(1.0f);

    b.add_output<nvrhi::TextureHandle>("Lit Color");
}

NODE_EXECUTION_FUNCTION(lighting_2d)
{
    auto& storage = params.get_storage<Lighting2DStorage&>();
    storage.rc = &(resource_allocator);

    auto normal_map = params.get_input<nvrhi::TextureHandle>("Normal Map");
    auto base_color = params.get_input<nvrhi::TextureHandle>("Base Color");
    auto light_dir = params.get_input<pxr::GfVec3f>("Light Direction");
    auto light_color = params.get_input<pxr::GfVec3f>("Light Color");
    float ambient = params.get_input<float>("Ambient");

    if (!normal_map) {
        spdlog::warn("lighting_2d: null normal map");
        return false;
    }

    int width = normal_map->getDesc().width;
    int height = normal_map->getDesc().height;

    // Create output texture
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.isUAV = true;
    desc.debugName = "LitColor";

    if (!storage.output ||
        storage.output->getDesc().width != width ||
        storage.output->getDesc().height != height) {
        storage.output = resource_allocator.device->createTexture(desc);
    }

    // TODO: Dispatch compute shader for Lambertian/Blinn-Phong lighting
    // Placeholder: fill with ambient light color
    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();

    nvrhi::Color clear_color(
        light_color[0] * ambient,
        light_color[1] * ambient,
        light_color[2] * ambient,
        1.0f);
    command_list->clearTextureFloat(
        storage.output, nvrhi::AllSubresources, clear_color);

    command_list->close();
    resource_allocator.device->executeCommandList(command_list);

    spdlog::info("lighting_2d: {}x{}, ambient={}", width, height, ambient);

    params.set_output("Lit Color", storage.output);
    return true;
}

NODE_DECLARATION_UI(lighting_2d);

NODE_DEF_CLOSE_SCOPE
