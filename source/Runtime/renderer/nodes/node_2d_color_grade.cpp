#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3f.h>

#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

struct ColorGrade2DStorage {
    constexpr static bool has_storage = false;

    nvrhi::TextureHandle output;
    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    ResourceAllocator* rc = nullptr;

    ~ColorGrade2DStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
        }
    }
};

NODE_DECLARATION_FUNCTION(color_grade_2d)
{
    b.add_input<nvrhi::TextureHandle>("Input");
    b.add_input<float>("Brightness").default_val(0.0f).min(-1.0f).max(1.0f);
    b.add_input<float>("Contrast").default_val(1.0f).min(0.0f).max(3.0f);
    b.add_input<float>("Saturation").default_val(1.0f).min(0.0f).max(3.0f);
    b.add_input<pxr::GfVec3f>("Tint");

    b.add_output<nvrhi::TextureHandle>("Graded");
}

NODE_EXECUTION_FUNCTION(color_grade_2d)
{
    auto& storage = params.get_storage<ColorGrade2DStorage&>();
    storage.rc = &(resource_allocator);

    auto input = params.get_input<nvrhi::TextureHandle>("Input");
    float brightness = params.get_input<float>("Brightness");
    float contrast = params.get_input<float>("Contrast");
    float saturation = params.get_input<float>("Saturation");
    auto tint = params.get_input<pxr::GfVec3f>("Tint");

    if (!input) {
        spdlog::warn("color_grade_2d: null input");
        return false;
    }

    int width = input->getDesc().width;
    int height = input->getDesc().height;

    // Create output texture
    auto desc = input->getDesc();
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.isUAV = true;
    desc.debugName = "GradedOutput";

    if (!storage.output ||
        storage.output->getDesc().width != width ||
        storage.output->getDesc().height != height) {
        storage.output = resource_allocator.device->createTexture(desc);
    }

    // TODO: Dispatch compute shader for color grading
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

    spdlog::info(
        "color_grade_2d: {}x{}, brightness={}, contrast={}, saturation={}",
        width, height, brightness, contrast, saturation);

    params.set_output("Graded", storage.output);
    return true;
}

NODE_DECLARATION_UI(color_grade_2d);

NODE_DEF_CLOSE_SCOPE
