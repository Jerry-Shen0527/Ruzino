#include "GPUContext/compute_context.hpp"
#include "RHI/internal/resources.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/utils/CameraParameters.h"
#include "shaders/utils/ray.slang"
#include "shaders/utils/view_cb.h"
#include "spdlog/spdlog.h"
#include "utils/cam_to_view_contants.h"
#include "utils/math.h"
#include "utils/resource_cleaner.hpp"

NODE_DEF_OPEN_SCOPE

struct RaygenStorage {
    constexpr static bool has_storage = false;

    float aperture = 0;
    float focus_distance = 2;

    PlanarViewConstants view_constants;

    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<ComputeContext> cached_compute_context;

    bool cached_scatter_rays = false;
    ProgramHandle cached_program;

    // Trantient data
    nvrhi::BufferHandle ray_buffer;
    nvrhi::BufferHandle pixel_target_buffer;

    GfVec2i image_size = GfVec2i(-1, -1);

    ResourceAllocator* rc = nullptr;

    ~RaygenStorage()
    {
        if (cached_program) {
            rc->destroy(cached_program);
            cached_program = nullptr;
        }
    }
};

NODE_DECLARATION_FUNCTION(node_render_ray_generation)
{
    b.add_input<nvrhi::TextureHandle>("random seeds");
    b.add_input<float>("Aperture").min(0).max(1).default_val(0);
    b.add_input<float>("Focus Distance").min(0).max(20).default_val(2);
    b.add_input<bool>("Scatter Rays").default_val(false);

    b.add_output<nvrhi::BufferHandle>("Pixel Target");
    b.add_output<nvrhi::BufferHandle>("Rays");
}

NODE_EXECUTION_FUNCTION(node_render_ray_generation)
{
    auto& g = params.get_global_payload<RenderGlobalPayload&>();
    auto& storage = params.get_storage<RaygenStorage&>();
    storage.rc = &resource_allocator;

    bool scatter_rays = params.get_input<bool>("Scatter Rays");
    if (scatter_rays != storage.cached_scatter_rays ||
        !storage.cached_program) {
        storage.cached_scatter_rays = scatter_rays;
        g.reset_accumulation = true;

        // Prepare the shader using reflection
        ProgramDesc cs_program_desc;
        cs_program_desc.shaderType = nvrhi::ShaderType::Compute;
        cs_program_desc.set_path("raygen.slang").set_entry_name("main");

        std::vector<ShaderMacro> macro_defines;
        if (params.get_input<bool>("Scatter Rays"))
            macro_defines.push_back(ShaderMacro{ "SCATTER_RAYS", "1" });
        else
            macro_defines.push_back(ShaderMacro{ "SCATTER_RAYS", "0" });

        cs_program_desc.define(macro_defines);

        if (storage.cached_program)
            resource_allocator.destroy(storage.cached_program);

        storage.cached_program = resource_allocator.create(cs_program_desc);
        CHECK_PROGRAM_ERROR(storage.cached_program);
    }

    Hd_RUZINO_Camera* free_camera = get_free_camera(params);
    auto view_constants = camera_to_view_constants(free_camera);

    bool view_changed = false;
    if (storage.view_constants != view_constants) {
        storage.view_constants = view_constants;
        view_changed = true;
    }

    auto image_size = free_camera->dataWindow.GetSize();

    bool size_changed = false;
    if (storage.image_size != image_size) {
        storage.image_size = image_size;
        size_changed = true;
    }

    auto aperture = params.get_input<float>("Aperture");
    auto focus_distance = params.get_input<float>("Focus Distance");

    bool camera_param_changed = false;
    if (storage.aperture != aperture ||
        storage.focus_distance != focus_distance) {
        storage.aperture = aperture;
        storage.focus_distance = focus_distance;
        camera_param_changed = true;
    }

    bool any_change = view_changed || size_changed || camera_param_changed;

    if (any_change) {
        g.reset_accumulation = true;
    }

    if (size_changed || !storage.ray_buffer)
        storage.ray_buffer = create_buffer<RayInfo>(
            params, image_size[0] * image_size[1], false, true);

    if (size_changed || !storage.pixel_target_buffer)
        storage.pixel_target_buffer = create_buffer<GfVec2i>(
            params, image_size[0] * image_size[1], false, true);

    if (any_change) {
        // spdlog::info(
        //     "Ray Generation Node: view changed: {}, size changed: {}, camera
        //     " "param changed: {}", view_changed, size_changed,
        //     camera_param_changed);
        auto random_seeds =
            params.get_input<nvrhi::TextureHandle>("random seeds");
        auto constant_buffer = get_free_camera_planarview_cb(params);
        MARK_DESTROY_NVRHI_RESOURCE(constant_buffer);

        storage.cached_program_vars = std::make_unique<ProgramVars>(
            resource_allocator, storage.cached_program);

        ProgramVars& program_vars = *storage.cached_program_vars;
        program_vars["rays"] = storage.ray_buffer;
        program_vars["random_seeds"] = random_seeds;
        program_vars["pixel_targets"] = storage.pixel_target_buffer;
        program_vars["viewConstant"] = constant_buffer;

        CameraParameters camera_params;
        camera_params.aperture = aperture;
        camera_params.focusDistance = focus_distance;

        auto camera_param_cb = create_constant_buffer(params, camera_params);
        MARK_DESTROY_NVRHI_RESOURCE(camera_param_cb);

        program_vars["cameraParams"] = camera_param_cb;

        program_vars.finish_setting_vars();

        storage.cached_compute_context =
            std::make_unique<ComputeContext>(resource_allocator, program_vars);
        ComputeContext& context = *storage.cached_compute_context;
        context.finish_setting_pso();
    }
    storage.cached_compute_context->begin();
    storage.cached_compute_context->dispatch(
        {}, *storage.cached_program_vars, image_size[0], 32, image_size[1], 32);
    storage.cached_compute_context->finish();

    params.set_output("Rays", storage.ray_buffer);
    params.set_output("Pixel Target", storage.pixel_target_buffer);
    return true;
}

NODE_DECLARATION_UI(node_render_ray_generation);
NODE_DEF_CLOSE_SCOPE
