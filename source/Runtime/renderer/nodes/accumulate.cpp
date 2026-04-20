
#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "utils/cam_to_view_contants.h"
#include "utils/view_cb.h"

NODE_DEF_OPEN_SCOPE
struct AccumulateStorage {
    constexpr static bool has_storage = false;

    int current_spp = 0;
    nvrhi::TextureHandle accumulated;

    PlanarViewConstants old_constants;
    pxr::GfVec2i image_size = pxr::GfVec2i(-1, -1);

    // Cached resources
    ProgramHandle cached_program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;
    nvrhi::BufferHandle cached_spp_cb;

    ResourceAllocator* rc = nullptr;

    ~AccumulateStorage()
    {
        if (rc && cached_program) {
            rc->destroy(cached_program);
            cached_program = nullptr;
        }
        if (rc && cached_spp_cb) {
            rc->destroy(cached_spp_cb);
            cached_spp_cb = nullptr;
        }
    }
};

NODE_DECLARATION_FUNCTION(accumulate)
{
    // Function content omitted
    b.add_input<nvrhi::TextureHandle>("Texture");
    b.add_input<int>("Max Samples").min(0).max(64).default_val(16);

    b.add_output<nvrhi::TextureHandle>("Accumulated");
}

NODE_EXECUTION_FUNCTION(accumulate)
{
    auto& storage = params.get_storage<AccumulateStorage&>();
    storage.rc = &(resource_allocator);

    // Create program if not cached
    if (!storage.cached_program ||
        storage.cached_program->get_desc().check_shader_updated()) {
        ProgramDesc cs_program_desc;
        cs_program_desc.shaderType = nvrhi::ShaderType::Compute;
        cs_program_desc.set_path("shaders/accumulate.slang")
            .set_entry_name("main");
        storage.cached_program = resource_allocator.create(cs_program_desc);
        CHECK_PROGRAM_ERROR(storage.cached_program);
    }

    auto texture = params.get_input<nvrhi::TextureHandle>("Texture");
    auto max_samples = params.get_input<int>("Max Samples");

    auto image_size =
        pxr::GfVec2i(texture->getDesc().width, texture->getDesc().height);

    // Check for size changes
    bool size_changed = (storage.image_size != image_size);
    if (size_changed) {
        storage.image_size = image_size;
    }

    // Check for texture descriptor changes or first time
    if (!storage.accumulated ||
        storage.accumulated->getDesc() != texture->getDesc()) {
        auto desc = texture->getDesc();

        storage.accumulated = resource_allocator.device->createTexture(desc);
        initialize_texture(
            params, storage.accumulated, nvrhi::Color{ 0, 0, 0, 1 });

        storage.current_spp = 0;
        size_changed = true;  // Force recreation of cached resources
    }

    // Check for view/camera changes
    bool view_changed = false;
    auto current_constants = camera_to_view_constants(get_free_camera(params));
    if (storage.old_constants != current_constants) {
        storage.current_spp = 0;
        initialize_texture(
            params, storage.accumulated, nvrhi::Color{ 0, 0, 0, 1 });
        storage.old_constants = current_constants;
        view_changed = true;
    }

    if (global_payload.reset_accumulation) {
        storage.current_spp = 0;
    }

    bool any_change = view_changed || size_changed;

    // Rebuild cached resources only when necessary
    if (any_change || !storage.cached_program_vars ||
        !storage.cached_compute_context) {
        storage.cached_program_vars = std::make_unique<ProgramVars>(
            resource_allocator, storage.cached_program);

        ProgramVars& program_vars = *storage.cached_program_vars;
        program_vars["Texture"] = texture;
        program_vars["Accumulated"] = storage.accumulated;

        // Create and cache the spp constant buffer
        if (storage.cached_spp_cb)
            resource_allocator.destroy(storage.cached_spp_cb);
        storage.cached_spp_cb =
            create_constant_buffer(params, storage.current_spp);
        program_vars["CurrentSPP"] = storage.cached_spp_cb;

        auto size_cb = create_constant_buffer(params, image_size);
        MARK_DESTROY_NVRHI_RESOURCE(size_cb);
        program_vars["ImageSize"] = size_cb;

        program_vars.finish_setting_vars();

        storage.cached_compute_context =
            std::make_unique<ComputeContext>(resource_allocator, program_vars);
        storage.cached_compute_context->finish_setting_pso();
    }

    // Update the constant buffer using CPU mapping
    auto ptr = resource_allocator.device->mapBuffer(
        storage.cached_spp_cb, nvrhi::CpuAccessMode::Write);
    memcpy(ptr, &storage.current_spp, sizeof(int));
    resource_allocator.device->unmapBuffer(storage.cached_spp_cb);

    // Execute compute shader using cached context
    storage.cached_compute_context->begin();
    storage.cached_compute_context->dispatch(
        {}, *storage.cached_program_vars, image_size[0], 32, image_size[1], 32);
    storage.cached_compute_context->finish();

    storage.current_spp++;

    params.set_output("Accumulated", storage.accumulated);
    return true;
}

NODE_DECLARATION_UI(accumulate);
NODE_DEF_CLOSE_SCOPE
