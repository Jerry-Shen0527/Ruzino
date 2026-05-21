#include <pxr/base/gf/vec2i.h>

#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

struct Blur2DStorage {
    constexpr static bool has_storage = false;

    nvrhi::TextureHandle output;
    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    ResourceAllocator* rc = nullptr;

    ~Blur2DStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
        }
    }
};

NODE_DECLARATION_FUNCTION(blur_2d)
{
    b.add_input<nvrhi::TextureHandle>("Input");
    b.add_input<float>("Radius").default_val(2.0f).min(0.0f).max(20.0f);
    b.add_input<int>("Passes").default_val(1).min(1).max(10);

    b.add_output<nvrhi::TextureHandle>("Blurred");
}

NODE_EXECUTION_FUNCTION(blur_2d)
{
    auto& storage = params.get_storage<Blur2DStorage&>();
    storage.rc = &(resource_allocator);

    auto input = params.get_input<nvrhi::TextureHandle>("Input");
    float radius = params.get_input<float>("Radius");
    int passes = params.get_input<int>("Passes");

    if (!input) {
        spdlog::warn("blur_2d: null input");
        return false;
    }

    int width = input->getDesc().width;
    int height = input->getDesc().height;

    // Create output texture matching input format
    auto desc = input->getDesc();
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.isUAV = true;
    desc.debugName = "BlurredOutput";

    if (!storage.output ||
        storage.output->getDesc().width != width ||
        storage.output->getDesc().height != height) {
        storage.output = resource_allocator.device->createTexture(desc);
    }

    // TODO: Dispatch separable Gaussian blur compute shader
    // Placeholder: copy input to output
    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();
    command_list->copyTexture(
        storage.output.Get(),
        nvrhi::TextureSlice(),
        input.Get(),
        nvrhi::TextureSlice());
    command_list->close();
    resource_allocator.device->executeCommandList(command_list);

    spdlog::info("blur_2d: {}x{}, radius={}, passes={}", width, height, radius, passes);

    params.set_output("Blurred", storage.output);
    return true;
}

NODE_DECLARATION_UI(blur_2d);

NODE_DEF_CLOSE_SCOPE
