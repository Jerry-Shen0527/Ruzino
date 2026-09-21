
#include "GPUContext/graphics_context.hpp"
#include "GPUContext/program_vars.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hgiGL/computeCmds.h"
#include "renderTLAS.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(rasterize)
{
    b.add_output<nvrhi::TextureHandle>("Position");
    b.add_output<nvrhi::TextureHandle>("Depth");
    b.add_output<nvrhi::TextureHandle>("Texcoords");
    b.add_output<nvrhi::TextureHandle>("DiffuseColor");
    b.add_output<nvrhi::TextureHandle>("MetallicRoughness");
    b.add_output<nvrhi::TextureHandle>("Normal");
    b.add_output<nvrhi::TextureHandle>("MaterialID");
}

// Persistent per-node state. The material-type LUT is rebuilt only when the
// scene's material version changes (mirrors how path_tracing.cpp rebuilds its
// RT program on DirtyMaterials).
struct RasterizeStorage
{
    static constexpr bool has_storage = false;

    nvrhi::BufferHandle materialTypeLUT;
    uint32_t cached_material_version = 0xFFFFFFFFu;
};

NODE_EXECUTION_FUNCTION(rasterize)
{
    std::vector<ShaderMacro> macros{ { "ENABLE_DIFFUSE_COLOR", "1" },
                                     { "ENABLE_METALLIC_ROUGHNESS", "1" },
                                     { "ENABLE_NORMAL", "1" },
                                     { "ENABLE_TEXCOORD", "1" } };

    ProgramDesc vs_program_desc;
    vs_program_desc.shaderType = nvrhi::ShaderType::Vertex;
    vs_program_desc.set_path("rasterize.vs.slang")
        .set_entry_name("main")
        .define(macros);

    ProgramHandle vs_program = resource_allocator.create(vs_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(vs_program);
    CHECK_PROGRAM_ERROR(vs_program);

    ProgramDesc ps_program_desc;
    ps_program_desc.shaderType = nvrhi::ShaderType::Pixel;
    ps_program_desc.set_path("rasterize.ps.slang")
        .set_entry_name("main")
        .define(macros);

    ProgramHandle ps_program = resource_allocator.create(ps_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(ps_program);
    CHECK_PROGRAM_ERROR(ps_program);

    auto view_cb = get_free_camera_planarview_cb(params);
    MARK_DESTROY_NVRHI_RESOURCE(view_cb);

    auto output_position = create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
    auto output_depth = create_default_depth_stencil(params);
    auto output_texcoords =
        create_default_render_target(params, nvrhi::Format::RG8_UNORM);
    // RGBA32_FLOAT to match the path tracer's output format (its readback via
    // get_output_texture works; the default RGBA16_FLOAT removed the device on
    // staging copy here).
    auto output_diffuse_color =
        create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
    auto output_metallic_roughness = create_default_render_target(params);
    auto output_normal = create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);
    // Per-pixel material id (pool index into materialHeaderBuffer), so the
    // deferred pass can run the real per-material BSDF. We use R32_FLOAT and
    // pack the id with asfloat/asuint (lossless) instead of R32_UINT, because
    // GraphicsContext::begin() clears color targets with clearTextureFloat,
    // which the validation layer rejects for integer textures.
    auto output_material_id =
        create_default_render_target(params, nvrhi::Format::R32_FLOAT);

    // ----------------------------------------------------------------------
    // Build / refresh the material-type LUT.
    //
    // MaterialHeader.material_type_id (stored on the GPU) is a *pool index*, not
    // the eval-callable enum (0=standard_surface, 1=UsdPreviewSurface,
    // 2=fallback, 3+=custom). The path tracer resolves pool-index -> enum via
    // the DXR fetch callables at runtime; the raster pipeline cannot CallShader
    // so it uploads a parallel LUT here instead.
    //
    // The enum is detected by scanning the MaterialX-generated fetch shader
    // source, which the generator emits as a literal
    //   "shader_type_id = 0;"  (standard_surface)
    //   "shader_type_id = 1;"  (UsdPreviewSurface)
    // (ClosureCompoundNodeSlang.cpp:222, 352, 403).
    // ----------------------------------------------------------------------
    auto& storage = params.get_storage<RasterizeStorage&>();
    uint32_t mat_version = instance_collection->get_material_version();

    const size_t lut_capacity =
        instance_collection->material_header_pool.pool_size();

    if (mat_version != storage.cached_material_version ||
        !storage.materialTypeLUT || lut_capacity == 0)
    {
        std::vector<uint32_t> cpu_lut(
            std::max<size_t>(lut_capacity, 1), 2u /* fallback */);

        auto& materials = global_payload.get_materials();
        for (auto& [path, mat] : materials)
        {
            if (!mat)
                continue;
            unsigned location = mat->GetMaterialLocation();
            if (location == (unsigned)-1 || location >= cpu_lut.size())
                continue;

            // Custom callable materials are not decodeable by the raster path's
            // shared functions; treat them as fallback for now.
            if (mat->HasValidShader())
            {
                cpu_lut[location] = 2u;
                continue;
            }

            std::string src = mat->GetShader(shader_factory);
            if (src.find("shader_type_id = 1") != std::string::npos)
                cpu_lut[location] = 1u;  // UsdPreviewSurface
            else if (src.find("shader_type_id = 0") != std::string::npos)
                cpu_lut[location] = 0u;  // standard_surface
            else
                cpu_lut[location] = 2u;  // fallback
            spdlog::info("[rasterize LUT] mat='{}' location={} -> type={} "
                         "(pool_size={}, src_has_t1={} src_has_t0={})",
                         path.GetText(), location, cpu_lut[location],
                         lut_capacity,
                         src.find("shader_type_id = 1") != std::string::npos,
                         src.find("shader_type_id = 0") != std::string::npos);
        }

        nvrhi::BufferDesc lut_desc;
        lut_desc.byteSize = cpu_lut.size() * sizeof(uint32_t);
        lut_desc.structStride = sizeof(uint32_t);
        lut_desc.initialState = nvrhi::ResourceStates::ShaderResource;
        lut_desc.debugName = "rasterize_materialTypeLUT";
        lut_desc.keepInitialState = true;
        if (storage.materialTypeLUT)
            resource_allocator.destroy(storage.materialTypeLUT);
        storage.materialTypeLUT = resource_allocator.create(lut_desc);

        // Upload via a command-list writeBuffer (the mapBuffer(Write) path left
        // the GPU reading zeros — a D3D12 upload-heap/visibility issue). This
        // mirrors how geometry/material data is uploaded elsewhere.
        auto lut_cl = resource_allocator.create(CommandListDesc{});
        MARK_DESTROY_NVRHI_RESOURCE(lut_cl);
        lut_cl->open();
        lut_cl->writeBuffer(storage.materialTypeLUT, cpu_lut.data(),
                            cpu_lut.size() * sizeof(uint32_t), 0);
        lut_cl->close();
        resource_allocator.device->executeCommandList(lut_cl);

        storage.cached_material_version = mat_version;
    }

    ProgramVars program_vars(resource_allocator, vs_program, ps_program);
    program_vars["viewConstant"] = view_cb;

    program_vars["instanceDescBuffer"] =
        instance_collection->instance_pool.get_device_buffer();
    program_vars["meshDescBuffer"] =
        instance_collection->mesh_pool.get_device_buffer();
    // volumeDescBuffer is declared unconditionally in
    // Scene/BindlessVertexBuffer.slang, so reflection always expects it — even
    // in scenes with no volumes. Leaving it unbound makes nvrhi report
    // "Bindings declared in the layout are not present in the binding set: t6"
    // and crashes the GPU (Device Removed). (Same hazard as path_tracing.cpp.)
    program_vars["volumeDescBuffer"] =
        instance_collection->volume_pool.get_device_buffer();

    program_vars["materialBlobBuffer"] =
        instance_collection->material_pool.get_device_buffer();
    program_vars["materialHeaderBuffer"] =
        instance_collection->material_header_pool.get_device_buffer();
    program_vars["materialTypeLUT"] = storage.materialTypeLUT;

    // Bind the bindless BUFFER table (VS imports BindlessVertexBuffer which
    // declares it). t_BindlessTextures is NOT imported by rasterize (it avoids
    // BindlessMaterial.slang to dodge the VS+PS root-signature overlap), so do
    // NOT bind it — a second finish_setting_vars() here previously destroyed
    // and rebuilt all binding sets, and binding a name absent from reflection
    // (t_BindlessTextures) is silently dropped. One finish, matching path_tracing.
    program_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    program_vars.finish_setting_vars();

    GraphicsContext context(resource_allocator, program_vars);
    context.set_render_target(0, output_position)
        .set_render_target(1, output_texcoords)
        .set_render_target(2, output_diffuse_color)
        .set_render_target(3, output_metallic_roughness)
        .set_render_target(4, output_normal)
        .set_render_target(5, output_material_id)
        .set_depth_stencil_target(output_depth)
        .finish_setting_frame_buffer();

    auto size = get_size(params);
    context.set_viewport(pxr::GfVec2f(size[0], size[1])).finish_setting_pso();

    instance_collection->draw_indirect_pool.compress();

    auto device_buffer =
        instance_collection->draw_indirect_pool.get_device_buffer();

    GraphicsRenderState state;

    context.begin();
    context.set_resource_state(device_buffer, ResourceStates::IndirectArgument);

    // Explicitly transition the material/scene SRV buffers into ShaderResource
    // state. These pools are written via writeBuffer on the Copy queue (see
    // DeviceMemoryPool::write_data) without a waitForIdle, so the graphics
    // queue must see a barrier before reading them as SRVs — otherwise the draw
    // may race ahead of the copy and read uninitialized (zero) data. The
    // buffers have keepInitialState=true so this is a one-way transition.
    auto blob_buf = instance_collection->material_pool.get_device_buffer();
    auto hdr_buf = instance_collection->material_header_pool.get_device_buffer();
    if (blob_buf)
        context.set_resource_state(blob_buf, ResourceStates::ShaderResource);
    if (hdr_buf)
        context.set_resource_state(hdr_buf, ResourceStates::ShaderResource);
    context.set_resource_state(
        instance_collection->instance_pool.get_device_buffer(),
        ResourceStates::ShaderResource);
    context.set_resource_state(
        instance_collection->mesh_pool.get_device_buffer(),
        ResourceStates::ShaderResource);
    context.set_resource_state(
        instance_collection->volume_pool.get_device_buffer(),
        ResourceStates::ShaderResource);
    context.set_resource_state(storage.materialTypeLUT,
                               ResourceStates::ShaderResource);

    context.draw_indirect(
        state,
        program_vars,
        device_buffer,
        instance_collection->draw_indirect_pool.count());

    context.finish();

    params.set_output("Position", output_position);
    params.set_output("Depth", output_depth);
    params.set_output("Texcoords", output_texcoords);
    params.set_output("DiffuseColor", output_diffuse_color);
    params.set_output("MetallicRoughness", output_metallic_roughness);
    params.set_output("Normal", output_normal);
    params.set_output("MaterialID", output_material_id);

    return true;
}

NODE_DECLARATION_UI(rasterize);
NODE_DEF_CLOSE_SCOPE
