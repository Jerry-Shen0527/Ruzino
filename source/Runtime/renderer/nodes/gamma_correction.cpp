#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "pxr/base/gf/vec2i.h"

NODE_DEF_OPEN_SCOPE
struct GammaCorrectionStorage {
    constexpr static bool has_storage = false;

    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    // Cached resources
    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    // Cached parameters to detect changes
    float cached_gamma = -1.0f;

    ResourceAllocator* rc = nullptr;

    ~GammaCorrectionStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
            cached_program = nullptr;
        }
    }
};

NODE_DECLARATION_FUNCTION(gamma_correction)
{
    b.add_input<nvrhi::TextureHandle>("Texture");
    b.add_input<float>("Gamma").min(0.1f).max(5.0f).default_val(2.2f);

    b.add_output<nvrhi::TextureHandle>("Corrected");
}

NODE_EXECUTION_FUNCTION(gamma_correction)
{
    auto& storage = params.get_storage<GammaCorrectionStorage&>();
    storage.rc = &resource_allocator;

    auto texture = params.get_input<nvrhi::TextureHandle>("Texture");
    auto gamma = params.get_input<float>("Gamma");

    auto image_size =
        pxr::GfVec2i(texture->getDesc().width, texture->getDesc().height);

    // Check for size changes
    bool size_changed = (storage.image_size != image_size);
    if (size_changed) {
        storage.image_size = image_size;
    }

    // Check for parameter changes
    bool params_changed = (storage.cached_gamma != gamma);

    // Create program if not cached
    if (!storage.cached_program) {
        ProgramDesc cs_program_desc;
        cs_program_desc.shaderType = nvrhi::ShaderType::Compute;
        cs_program_desc.set_path("gamma_correction.slang")
            .set_entry_name("main");
        storage.cached_program = resource_allocator.create(cs_program_desc);
        CHECK_PROGRAM_ERROR(storage.cached_program);
    }

    // Create output texture
    auto desc = texture->getDesc();
    auto corrected_texture = resource_allocator.create(desc);

    bool any_change = size_changed || params_changed;

    // Rebuild cached resources only when necessary
    if (any_change || !storage.cached_program_vars ||
        !storage.cached_compute_context) {
        // Update cached parameters
        storage.cached_gamma = gamma;

        // Create program vars
        storage.cached_program_vars = std::make_unique<ProgramVars>(
            resource_allocator, storage.cached_program);

        ProgramVars& program_vars = *storage.cached_program_vars;
        program_vars["InputTexture"] = texture;
        program_vars["OutputTexture"] = corrected_texture;

        auto gamma_cb = create_constant_buffer(params, gamma);
        MARK_DESTROY_NVRHI_RESOURCE(gamma_cb);
        program_vars["Gamma"] = gamma_cb;

        auto size_cb = create_constant_buffer(params, image_size);
        MARK_DESTROY_NVRHI_RESOURCE(size_cb);
        program_vars["ImageSize"] = size_cb;

        program_vars.finish_setting_vars();

        // Create compute context
        storage.cached_compute_context =
            std::make_unique<ComputeContext>(resource_allocator, program_vars);
        storage.cached_compute_context->finish_setting_pso();
    }

    // Execute compute shader
    storage.cached_compute_context->begin();
    storage.cached_compute_context->dispatch(
        {}, *storage.cached_program_vars, image_size[0], 32, image_size[1], 32);
    storage.cached_compute_context->finish();

    params.set_output("Corrected", corrected_texture);
    return true;
}

NODE_DECLARATION_UI(gamma_correction);
NODE_DEF_CLOSE_SCOPE
