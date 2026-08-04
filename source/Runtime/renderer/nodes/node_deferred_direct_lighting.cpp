// node_deferred_direct_lighting.cpp
//
// Deferred direct-lighting compute node for the rasterization pipeline.
//
//   rasterize (G-Buffer) ──► deferred_direct_lighting ──► present_color
//
// Uses the manual binding path (shader_factory.compile_shader + explicit
// BindingSetDesc + ComputePipelineDesc + command_list->setComputeState), mirroring
// the proven pattern in node_render_taa.cpp. The reflection-driven ProgramVars +
// ComputeContext path crashed on dispatch (device removed) even for a constant-
// output CS, so we use the explicit-slot approach that TAA/RNG nodes use.
//
// Registration: add_nodes() globs this into node_deferred_direct_lighting.dll
// and regenerates render_nodes.json automatically (AddNodes.cmake).

#include "GPUContext/program_vars.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "nvrhi/utils.h"
#include "renderTLAS.h"
#include "shaders/shaders/utils/view_cb.h"
#include "spdlog/spdlog.h"
#include "utils/math.h"

NODE_DEF_OPEN_SCOPE

struct DeferredLightingConstants
{
    uint32_t lightCount;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

NODE_DECLARATION_FUNCTION(deferred_direct_lighting)
{
    // G-Buffer inputs (mirror rasterize node outputs).
    b.add_input<nvrhi::TextureHandle>("Position");
    b.add_input<nvrhi::TextureHandle>("Texcoords");
    b.add_input<nvrhi::TextureHandle>("DiffuseColor");
    b.add_input<nvrhi::TextureHandle>("MetallicRoughness");
    b.add_input<nvrhi::TextureHandle>("Normal");
    b.add_input<nvrhi::TextureHandle>("MaterialID");

    b.add_output<nvrhi::TextureHandle>("Color");
}

NODE_EXECUTION_FUNCTION(deferred_direct_lighting)
{
    auto g_pos = params.get_input<nvrhi::TextureHandle>("Position");
    auto g_normal = params.get_input<nvrhi::TextureHandle>("Normal");
    if (!g_pos || !g_normal) {
        spdlog::warn("deferred_direct_lighting: missing G-Buffer inputs");
        return false;
    }

    auto g_texcoord = params.get_input<nvrhi::TextureHandle>("Texcoords");
    auto g_diffuse = params.get_input<nvrhi::TextureHandle>("DiffuseColor");
    auto g_mr =
        params.get_input<nvrhi::TextureHandle>("MetallicRoughness");
    auto g_matid = params.get_input<nvrhi::TextureHandle>("MaterialID");

    auto size = get_size(params);

    // --- Output texture (linear RGBA16F) ---
    nvrhi::TextureDesc out_desc;
    out_desc.width = size[0];
    out_desc.height = size[1];
    out_desc.format = nvrhi::Format::RGBA16_FLOAT;
    out_desc.dimension = nvrhi::TextureDimension::Texture2D;
    out_desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    out_desc.keepInitialState = true;
    out_desc.isUAV = true;
    out_desc.debugName = "DeferredLitColor";
    auto output = resource_allocator.create(out_desc);
    MARK_DESTROY_NVRHI_RESOURCE(output);

    // --- Compile compute shader ---
    std::string error_string;
    ShaderReflectionInfo reflection;
    auto cs_shader = shader_factory.compile_shader(
        "main",
        nvrhi::ShaderType::Compute,
        "shaders/deferred_direct_lighting.cs.slang",
        reflection,
        error_string);
    MARK_DESTROY_NVRHI_RESOURCE(cs_shader);
    if (!cs_shader || !error_string.empty()) {
        spdlog::error("deferred_direct_lighting CS compile failed: {}",
                      error_string);
        return false;
    }

    nvrhi::BindingLayoutDescVector layout_descs =
        reflection.get_binding_layout_descs();
    auto binding_layout = resource_allocator.create(layout_descs[0]);
    MARK_DESTROY_NVRHI_RESOURCE(binding_layout);

    nvrhi::ComputePipelineDesc pipeline_desc;
    pipeline_desc.CS = cs_shader;
    pipeline_desc.bindingLayouts = { binding_layout };
    auto compute_pipeline = resource_allocator.create(pipeline_desc);
    MARK_DESTROY_NVRHI_RESOURCE(compute_pipeline);

    // --- View + per-pass constants ---
    auto view_cb = get_free_camera_planarview_cb(params);
    MARK_DESTROY_NVRHI_RESOURCE(view_cb);

    instance_collection->light_pool.compress();
    uint32_t lightCount =
        static_cast<uint32_t>(instance_collection->light_pool.count());
    DeferredLightingConstants constants{ lightCount, 0, 0, 0 };
    auto constants_cb = create_constant_buffer(params, constants);
    MARK_DESTROY_NVRHI_RESOURCE(constants_cb);

    // --- Material-type LUT ---
    const size_t lut_capacity =
        instance_collection->material_header_pool.pool_size();
    std::vector<uint32_t> cpu_lut(
        std::max<size_t>(lut_capacity, 1), 2u /* fallback */);
    auto& materials = global_payload.get_materials();
    for (auto& [path, mat] : materials) {
        if (!mat)
            continue;
        unsigned location = mat->GetMaterialLocation();
        if (location == (unsigned)-1 || location >= cpu_lut.size())
            continue;
        if (mat->HasValidShader()) {
            cpu_lut[location] = 2u;
            continue;
        }
        std::string src = mat->GetShader(shader_factory);
        if (src.find("shader_type_id = 1") != std::string::npos)
            cpu_lut[location] = 1u;
        else if (src.find("shader_type_id = 0") != std::string::npos)
            cpu_lut[location] = 0u;
        else
            cpu_lut[location] = 2u;
    }
    nvrhi::BufferDesc lut_desc;
    lut_desc.byteSize = cpu_lut.size() * sizeof(uint32_t);
    lut_desc.structStride = sizeof(uint32_t);
    lut_desc.initialState = nvrhi::ResourceStates::ShaderResource;
    lut_desc.debugName = "deferred_materialTypeLUT";
    lut_desc.keepInitialState = true;
    lut_desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    auto material_type_lut = resource_allocator.create(lut_desc);
    MARK_DESTROY_NVRHI_RESOURCE(material_type_lut);
    {
        auto mapped = resource_allocator.device->mapBuffer(
            material_type_lut, nvrhi::CpuAccessMode::Write);
        memcpy(mapped, cpu_lut.data(), cpu_lut.size() * sizeof(uint32_t));
        resource_allocator.device->unmapBuffer(material_type_lut);
    }

    // --- Binding set (explicit slots, TAA-style) ---
    // Slot order must match the shader declarations in
    // deferred_direct_lighting.cs.slang. The bindless buffer table is not
    // declared by the CS (it reads material data via the named structured
    // buffers), so only named resources appear here.
    nvrhi::BindingSetDesc binding_set_desc;
    binding_set_desc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(0, g_pos),
        nvrhi::BindingSetItem::Texture_SRV(1, g_texcoord),
        nvrhi::BindingSetItem::Texture_SRV(2, g_diffuse),
        nvrhi::BindingSetItem::Texture_SRV(3, g_mr),
        nvrhi::BindingSetItem::Texture_SRV(4, g_normal),
        nvrhi::BindingSetItem::Texture_SRV(5, g_matid),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(
            6, instance_collection->light_pool.get_device_buffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(
            7, instance_collection->material_header_pool.get_device_buffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, material_type_lut),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(
            9, instance_collection->material_pool.get_device_buffer()),
        nvrhi::BindingSetItem::ConstantBuffer(0, view_cb),
        nvrhi::BindingSetItem::Texture_UAV(0, output),
        nvrhi::BindingSetItem::ConstantBuffer(1, constants_cb),
    };
    auto binding_set =
        resource_allocator.create(binding_set_desc, binding_layout);
    MARK_DESTROY_NVRHI_RESOURCE(binding_set);

    // --- Dispatch ---
    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();
    // Transition G-Buffer textures out of their RenderTarget state (left by the
    // rasterize node's graphics pass) into ShaderResource, and the output UAV
    // into UnorderedAccess. The textures are created with keepInitialState=true
    // so nvrhi won't auto-transition; an explicit texture-state barrier is
    // required or the GPU reads a stale resource state and the device is removed.
    command_list->setTextureState(g_pos, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(g_texcoord, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(g_diffuse, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(g_mr, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(g_normal, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(g_matid, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(output, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
    // Also transition the scratch/scene buffers the CS reads.
    command_list->setBufferState(
        instance_collection->light_pool.get_device_buffer(),
        nvrhi::ResourceStates::ShaderResource);
    command_list->setBufferState(
        instance_collection->material_header_pool.get_device_buffer(),
        nvrhi::ResourceStates::ShaderResource);
    command_list->setBufferState(
        material_type_lut, nvrhi::ResourceStates::ShaderResource);
    command_list->setBufferState(
        instance_collection->material_pool.get_device_buffer(),
        nvrhi::ResourceStates::ShaderResource);
    nvrhi::ComputeState compute_state;
    compute_state.pipeline = compute_pipeline;
    compute_state.bindings = { binding_set };
    command_list->setComputeState(compute_state);
    command_list->dispatch(div_ceil(size[0], 8), div_ceil(size[1], 8));
    command_list->close();
    resource_allocator.device->executeCommandList(command_list);

    params.set_output("Color", output);
    return true;
}

NODE_DECLARATION_UI(deferred_direct_lighting);
NODE_DEF_CLOSE_SCOPE
