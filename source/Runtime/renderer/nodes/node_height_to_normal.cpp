#include <pxr/base/gf/vec2i.h>

#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

struct HeightToNormalStorage {
    constexpr static bool has_storage = false;

    nvrhi::TextureHandle normal_map;
    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    ResourceAllocator* rc = nullptr;

    ~HeightToNormalStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
        }
    }
};

NODE_DECLARATION_FUNCTION(height_to_normal)
{
    b.add_input<nvrhi::TextureHandle>("Height Map");
    b.add_input<float>("Strength").default_val(1.0f).min(0.01f).max(10.0f);

    b.add_output<nvrhi::TextureHandle>("Normal Map");
}

NODE_EXECUTION_FUNCTION(height_to_normal)
{
    auto& storage = params.get_storage<HeightToNormalStorage&>();
    storage.rc = &(resource_allocator);

    auto height_map = params.get_input<nvrhi::TextureHandle>("Height Map");
    float strength = params.get_input<float>("Strength");

    if (!height_map) {
        spdlog::warn("height_to_normal: null input");
        return false;
    }

    int width = height_map->getDesc().width;
    int height = height_map->getDesc().height;
    auto image_size = pxr::GfVec2i(width, height);

    // Create output normal map texture
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.isUAV = true;
    desc.debugName = "NormalMap";

    bool size_changed = (storage.image_size != image_size);
    if (size_changed) {
        storage.image_size = image_size;
    }

    if (!storage.normal_map ||
        storage.normal_map->getDesc().width != width ||
        storage.normal_map->getDesc().height != height) {
        storage.normal_map = resource_allocator.device->createTexture(desc);
        size_changed = true;
    }

    // TODO: Create and dispatch compute shader for Sobel-operator-based normal computation
    // Placeholder: fill with flat up-facing normals
    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();

    nvrhi::Color clear_color(0.5f, 0.5f, 1.0f, 1.0f);
    command_list->clearTextureFloat(
        storage.normal_map, nvrhi::AllSubresources, clear_color);

    command_list->close();
    resource_allocator.device->executeCommandList(command_list);

    spdlog::info("height_to_normal: {}x{}, strength={}", width, height, strength);

    params.set_output("Normal Map", storage.normal_map);
    return true;
}

NODE_DECLARATION_UI(height_to_normal);

NODE_DEF_CLOSE_SCOPE
