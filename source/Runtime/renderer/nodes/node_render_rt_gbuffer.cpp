#include <pxr/base/gf/vec2i.h>
#include <spdlog/spdlog.h>

#include <memory>

#include "../source/renderTLAS.h"
#include "GPUContext/program_vars.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "RHI/shaderCompiler.h"
#include "camera.h"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/utils/ray.slang"

// Standalone raytracing G-buffer node: fires one primary ray per pixel
// through the same TLAS / vertex-interpolation path as the path tracer and
// outputs world Normal / Position / Texcoords / Tangent as RGBA32F
// textures. Geometry-only ground truth used to cross-check the rasterize
// G-buffer; deliberately independent from the path_tracing node.

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(rt_gbuffer)
{
    b.add_input<nvrhi::BufferHandle>("Pixel Target");
    b.add_input<nvrhi::BufferHandle>("Rays");

    b.add_output<nvrhi::TextureHandle>("Normal");
    b.add_output<nvrhi::TextureHandle>("Position");
    b.add_output<nvrhi::TextureHandle>("Texcoords");
    b.add_output<nvrhi::TextureHandle>("Tangent");

    // Function content omitted
}

struct RtGBufferStorage {
    constexpr static bool has_storage = false;
    GfVec2i old_size = GfVec2i(-1, -1);

    ProgramHandle program;
    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<RaytracingContext> cached_rt_context;
    ResourceAllocator* rc = nullptr;

    nvrhi::BufferHandle normal_buf;
    nvrhi::BufferHandle position_buf;
    nvrhi::BufferHandle uv_buf;
    nvrhi::BufferHandle tangent_buf;
    nvrhi::BufferHandle constants_buffer;

    nvrhi::TextureHandle normal_tex;
    nvrhi::TextureHandle position_tex;
    nvrhi::TextureHandle uv_tex;
    nvrhi::TextureHandle tangent_tex;

    size_t pixel_count = 0;

    ~RtGBufferStorage()
    {
        if (!rc)
            return;
        rc->destroy(program);
        rc->destroy(normal_buf);
        rc->destroy(position_buf);
        rc->destroy(uv_buf);
        rc->destroy(tangent_buf);
        rc->destroy(constants_buffer);
        rc->destroy(normal_tex);
        rc->destroy(position_tex);
        rc->destroy(uv_tex);
        rc->destroy(tangent_tex);
        // cached_program_vars / cached_rt_context release their own handles
        // through the same allocator on destruction.
    }
};

NODE_EXECUTION_FUNCTION(rt_gbuffer)
{
    using namespace nvrhi;

    auto& storage = params.get_storage<RtGBufferStorage&>();
    storage.rc = &(resource_allocator);

    auto size = get_free_camera(params)->dataWindow.GetSize();
    bool size_changed = (storage.old_size != size);
    storage.old_size = size;

    // Geometry-only node: materials and lights are irrelevant, but the
    // TLAS / instance pools change with geometry.
    auto& g = global_payload;
    const bool geom_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);

    const size_t pixel_count =
        static_cast<size_t>(size[0]) * static_cast<size_t>(size[1]);

    if (size_changed || !storage.program) {
        ProgramDesc program_desc;
        program_desc.set_path("rt_gbuffer.slang");
        program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;
        if (storage.program)
            resource_allocator.destroy(storage.program);
        storage.program = resource_allocator.create(program_desc);
        CHECK_PROGRAM_ERROR(storage.program);
    }

    if (size_changed || storage.pixel_count != pixel_count ||
        !storage.normal_tex) {
        const auto make_gbuffer_buffer = [&](const char* name) {
            return resource_allocator.create(
                nvrhi::BufferDesc{}
                    .setByteSize(pixel_count * sizeof(float) * 4)
                    .setStructStride(sizeof(float) * 4)
                    .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                    .setKeepInitialState(true)
                    .setCanHaveUAVs(true)
                    .setDebugName(name));
        };
        if (storage.normal_buf)
            resource_allocator.destroy(storage.normal_buf);
        if (storage.position_buf)
            resource_allocator.destroy(storage.position_buf);
        if (storage.uv_buf)
            resource_allocator.destroy(storage.uv_buf);
        if (storage.tangent_buf)
            resource_allocator.destroy(storage.tangent_buf);
        storage.normal_buf = make_gbuffer_buffer("rtg_normal");
        storage.position_buf = make_gbuffer_buffer("rtg_position");
        storage.uv_buf = make_gbuffer_buffer("rtg_uv");
        storage.tangent_buf = make_gbuffer_buffer("rtg_tangent");

        // create_default_render_target follows the current camera viewport,
        // so a resize strands the old textures in the allocator unless they
        // are returned first (same rent/return discipline as the buffers
        // above; the dtor only ever sees the last set).
        if (storage.normal_tex)
            resource_allocator.destroy(storage.normal_tex);
        if (storage.position_tex)
            resource_allocator.destroy(storage.position_tex);
        if (storage.uv_tex)
            resource_allocator.destroy(storage.uv_tex);
        if (storage.tangent_tex)
            resource_allocator.destroy(storage.tangent_tex);
        storage.normal_tex =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
        storage.position_tex =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
        storage.uv_tex =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
        storage.tangent_tex =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);

        storage.pixel_count = pixel_count;
    }

    // Rebind all scene-dependent bindings on geometry change or size
    // change (a debug node -- rebuilding ProgramVars is cheap enough and
    // keeps the binding set trivially correct).
    if (size_changed || geom_dirty || !storage.cached_program_vars ||
        !storage.cached_rt_context) {
        storage.cached_program_vars =
            std::make_unique<ProgramVars>(resource_allocator, storage.program);
        ProgramVars& program_vars = *storage.cached_program_vars;

        program_vars["SceneBVH"] =
            params.get_global_payload<RenderGlobalPayload&>()
                .InstanceCollection->get_tlas();
        program_vars["inPixelTarget"] =
            params.get_input<nvrhi::BufferHandle>("Pixel Target");
        program_vars["rays"] = params.get_input<nvrhi::BufferHandle>("Rays");
        program_vars["gbuffer_normal"] = storage.normal_buf;
        program_vars["gbuffer_position"] = storage.position_buf;
        program_vars["gbuffer_uv"] = storage.uv_buf;
        program_vars["gbuffer_tangent"] = storage.tangent_buf;

        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();
        program_vars["volumeDescBuffer"] =
            instance_collection->volume_pool.get_device_buffer();

        struct RtgConstants {
            uint32_t width;
            uint32_t height;
        };
        RtgConstants constants;
        constants.width = static_cast<uint32_t>(size[0]);
        constants.height = static_cast<uint32_t>(size[1]);
        if (storage.constants_buffer)
            resource_allocator.destroy(storage.constants_buffer);
        storage.constants_buffer = create_constant_buffer(params, constants);
        program_vars["rtgConstants"] = storage.constants_buffer;

        // get_interpolated_vertex fetches vertices through the bindless
        // buffer table.
        program_vars.set_descriptor_table(
            "t_BindlessBuffers",
            instance_collection->bindlessData.bufferDescriptorTableManager
                ->GetDescriptorTable(),
            instance_collection->bindlessData.bufferBindlessLayout);

        program_vars.finish_setting_vars();

        storage.cached_rt_context = std::make_unique<RaytracingContext>(
            resource_allocator, *storage.cached_program_vars);

        RaytracingContext& context = *storage.cached_rt_context;
        context.announce_raygeneration("RayGen");
        context.announce_hitgroup("ClosestHit", "", "", 0);
        context.announce_miss("Miss", 0);
        context.finish_announcing_shader_names();
    }

    auto rays = params.get_input<nvrhi::BufferHandle>("Rays");
    if (!rays) {
        // Unwired Rays: nothing to trace. Outputs still expose the last
        // cook's textures (or fresh empty ones after a resize) so a
        // half-wired graph keeps rendering something downstream.
        spdlog::warn("rt_gbuffer: Rays input is not wired, skipping trace");
        params.set_output("Normal", storage.normal_tex);
        params.set_output("Position", storage.position_tex);
        params.set_output("Texcoords", storage.uv_tex);
        params.set_output("Tangent", storage.tangent_tex);
        return true;
    }
    auto buffer_size = rays->getDesc().byteSize / sizeof(RayInfo);

    if (buffer_size > 0) {
        storage.cached_rt_context->begin();
        storage.cached_rt_context->trace_rays(
            {}, *storage.cached_program_vars, buffer_size, 1, 1);
        storage.cached_rt_context->finish();

        // Read the G-buffer arrays back and upload into the output
        // textures (debug node; the readback is the point).
        const size_t bytes = pixel_count * sizeof(float) * 4;
        const auto readback = [&](nvrhi::BufferHandle src, float* dst) {
            auto rb = resource_allocator.create(
                nvrhi::BufferDesc{}
                    .setByteSize(bytes)
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("rtg_readback"));
            auto cmd = resource_allocator.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(rb, 0, src, 0, bytes);
            cmd->close();
            RHI::get_device()->executeCommandList(cmd);
            RHI::get_device()->waitForIdle();
            void* mapped =
                RHI::get_device()->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(dst, mapped, bytes);
            RHI::get_device()->unmapBuffer(rb);
            resource_allocator.destroy(rb);
            resource_allocator.destroy(cmd);
        };

        std::vector<float> normal_data(pixel_count * 4);
        std::vector<float> position_data(pixel_count * 4);
        std::vector<float> uv_data(pixel_count * 4);
        std::vector<float> tangent_data(pixel_count * 4);
        readback(storage.normal_buf, normal_data.data());
        readback(storage.position_buf, position_data.data());
        readback(storage.uv_buf, uv_data.data());
        readback(storage.tangent_buf, tangent_data.data());

        const auto upload = [&](nvrhi::TextureHandle tex, const float* data) {
            auto cmd = resource_allocator.create(CommandListDesc{});
            cmd->open();
            cmd->writeTexture(
                tex,
                0,
                0,
                data,
                static_cast<size_t>(size[0]) * sizeof(float) * 4);
            cmd->close();
            RHI::get_device()->executeCommandList(cmd);
            RHI::get_device()->waitForIdle();
            resource_allocator.destroy(cmd);
        };
        upload(storage.normal_tex, normal_data.data());
        upload(storage.position_tex, position_data.data());
        upload(storage.uv_tex, uv_data.data());
        upload(storage.tangent_tex, tangent_data.data());
    }

    params.set_output("Normal", storage.normal_tex);
    params.set_output("Position", storage.position_tex);
    params.set_output("Texcoords", storage.uv_tex);
    params.set_output("Tangent", storage.tangent_tex);

    return true;
}

NODE_DEF_CLOSE_SCOPE
