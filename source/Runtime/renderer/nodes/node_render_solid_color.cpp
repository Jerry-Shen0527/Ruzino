#include "camera.h"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(solid_color)
{
    b.add_input<float>("R").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("G").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("B").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("A").default_val(1.0f).min(0.0f).max(1.0f);

    b.add_output<nvrhi::TextureHandle>("Color");
}

NODE_EXECUTION_FUNCTION(solid_color)
{
    float r = params.get_input<float>("R");
    float g = params.get_input<float>("G");
    float b = params.get_input<float>("B");
    float a = params.get_input<float>("A");

    Hd_RUZINO_Camera* free_camera = get_free_camera(params);
    auto size = free_camera->dataWindow.GetSize();

    nvrhi::TextureDesc desc;
    desc.width = size[0];
    desc.height = size[1];
    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.isUAV = true;
    desc.debugName = "SolidColor";

    auto texture = resource_allocator.create(desc);

    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();
    command_list->clearTextureFloat(
        texture, nvrhi::AllSubresources, nvrhi::Color(r, g, b, a));
    command_list->close();
    resource_allocator.device->executeCommandList(command_list);

    spdlog::info("solid_color: {}x{}, rgba=({},{},{},{})", size[0], size[1], r, g, b, a);

    params.set_output("Color", texture);
    return true;
}

NODE_DECLARATION_UI(solid_color);

NODE_DEF_CLOSE_SCOPE
