// node_volumetric_fog_composite.cpp
//
// Real-time volumetric fog composite node for the rasterization pipeline.
//
//   rasterize (G-Buffer) ──► deferred_direct_lighting ──►
//   volumetric_fog_composite ──► present_color
//                                (Color)  (Position) ────────┘
//                                          (from rasterize)
//
// Marches an exponential height fog through the deferred-lit color and fills
// background pixels with the Hosek-Wilkie sky (see
// volumetric_fog_composite.cs.slang). Uses the manual binding path
// (shader_factory.compile_shader + explicit BindingSetDesc), the proven
// TAA/deferred pattern — the reflection-driven ProgramVars path crashed on
// dispatch.
//
// Registration: add_nodes() globs this into node_volumetric_fog_composite.dll
// and regenerates render_nodes.json automatically (AddNodes.cmake).

#include <pxr/base/gf/vec3f.h>

#include <algorithm>

// Local ValueTrait specializations (node_lpm.cpp pattern) so the vec3 socket
// supports .default_val().
#include "nodes/core/socket_trait.inl"
template<>
struct ValueTrait<pxr::GfVec3f> {
    static constexpr bool has_min = false;
    static constexpr bool has_max = false;
    static constexpr bool has_default = true;
};

#include "GPUContext/program_vars.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "nvrhi/utils.h"
#include "renderTLAS.h"
#include "shaders/utils/view_cb.h"
#include "sky/hosek_sky_model.h"
#include "spdlog/spdlog.h"
#include "utils/math.h"

NODE_DEF_OPEN_SCOPE

// Must match the FogConstants cbuffer layout in
// volumetric_fog_composite.cs.slang (HLSL packing: 4x float, then
// float3+float, then 2x uint + 2x float).
struct VolumetricFogConstants {
    float density;
    float heightFalloff;
    float groundHeight;
    float phaseG;
    float scatterAlbedo[3];
    float maxDistance;
    uint32_t lightCount;
    uint32_t skyBackground;
    float pad0;
    float pad1;
};

NODE_DECLARATION_FUNCTION(volumetric_fog_composite)
{
    b.add_input<nvrhi::TextureHandle>("Color");
    b.add_input<nvrhi::TextureHandle>("Position");

    b.add_input<float>("Density").default_val(0.02f).min(0.0f).max(1.0f);
    b.add_input<float>("Height Falloff")
        .default_val(0.5f)
        .min(0.001f)
        .max(100.0f);
    b.add_input<float>("Ground Height").default_val(0.0f);
    b.add_input<float>("Phase G").default_val(0.3f).min(-0.99f).max(0.99f);
    b.add_input<pxr::GfVec3f>("Scattering Albedo")
        .default_val(pxr::GfVec3f(0.9f, 0.9f, 0.9f));
    b.add_input<float>("Max Distance")
        .default_val(200.0f)
        .min(1.0f)
        .max(10000.0f);
    b.add_input<bool>("Sky Background").default_val(true);

    b.add_output<nvrhi::TextureHandle>("Color");
}

NODE_EXECUTION_FUNCTION(volumetric_fog_composite)
{
    auto scene_color = params.get_input<nvrhi::TextureHandle>("Color");
    auto g_pos = params.get_input<nvrhi::TextureHandle>("Position");
    if (!scene_color || !g_pos) {
        spdlog::warn("volumetric_fog_composite: missing inputs");
        return false;
    }

    float density = params.get_input<float>("Density");
    float height_falloff = params.get_input<float>("Height Falloff");
    float ground_height = params.get_input<float>("Ground Height");
    float phase_g = params.get_input<float>("Phase G");
    auto albedo_v = params.get_input<pxr::GfVec3f>("Scattering Albedo");
    float max_distance = params.get_input<float>("Max Distance");
    bool sky_background = params.get_input<bool>("Sky Background");

    auto size = get_size(params);

    // --- Output texture (RGBA32_FLOAT to match get_output_texture's hardcoded
    // 16-bytes/pixel readback assumption) ---
    nvrhi::TextureDesc out_desc;
    out_desc.width = size[0];
    out_desc.height = size[1];
    out_desc.format = nvrhi::Format::RGBA32_FLOAT;
    out_desc.dimension = nvrhi::TextureDimension::Texture2D;
    out_desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    out_desc.keepInitialState = true;
    out_desc.isUAV = true;
    out_desc.debugName = "VolumetricFogColor";
    auto output = resource_allocator.create(out_desc);
    MARK_DESTROY_NVRHI_RESOURCE(output);

    // --- Compile compute shader ---
    std::string error_string;
    ShaderReflectionInfo reflection;
    auto cs_shader = shader_factory.compile_shader(
        "main",
        nvrhi::ShaderType::Compute,
        "volumetric_fog_composite.cs.slang",
        reflection,
        error_string);
    MARK_DESTROY_NVRHI_RESOURCE(cs_shader);
    if (!cs_shader || !error_string.empty()) {
        spdlog::error(
            "volumetric_fog_composite CS compile failed: {}", error_string);
        return false;
    }

    nvrhi::BindingLayoutDescVector layout_descs =
        reflection.get_binding_layout_descs();
    if (layout_descs.empty()) {
        spdlog::error(
            "volumetric_fog_composite: shader reflection returned no "
            "binding layouts");
        return false;
    }
    auto binding_layout = resource_allocator.create(layout_descs[0]);
    MARK_DESTROY_NVRHI_RESOURCE(binding_layout);

    nvrhi::ComputePipelineDesc pipeline_desc;
    pipeline_desc.CS = cs_shader;
    pipeline_desc.bindingLayouts = { binding_layout };
    auto compute_pipeline = resource_allocator.create(pipeline_desc);
    MARK_DESTROY_NVRHI_RESOURCE(compute_pipeline);

    // --- View + fog constants ---
    auto view_cb = get_free_camera_planarview_cb(params);
    MARK_DESTROY_NVRHI_RESOURCE(view_cb);

    instance_collection->light_pool.compress();
    uint32_t lightCount =
        static_cast<uint32_t>(instance_collection->light_pool.count());

    VolumetricFogConstants constants{ density,
                                      height_falloff,
                                      ground_height,
                                      phase_g,
                                      { std::clamp(albedo_v[0], 0.0f, 1.0f),
                                        std::clamp(albedo_v[1], 0.0f, 1.0f),
                                        std::clamp(albedo_v[2], 0.0f, 1.0f) },
                                      max_distance,
                                      lightCount,
                                      sky_background ? 1u : 0u,
                                      0.0f,
                                      0.0f };
    auto constants_cb = create_constant_buffer(params, constants);
    MARK_DESTROY_NVRHI_RESOURCE(constants_cb);

    // --- Hosek state buffer: the shader declares it unconditionally, so the
    // pool must own a device buffer even in scenes with no Hosek dome.
    // Reserve the zeroed dummy row 0 (the "hosekStateIndex = 0" invariant;
    // light.cpp reserves it too on the first cook) and keep its handle alive
    // on the collection so the row is never freed/reused. ---
    auto& hosek_pool = instance_collection->hosek_state_pool;
    if (!hosek_pool.get_device_buffer()) {
        ruzino::HosekSkyState zero{};
        instance_collection->hosek_dummy_row = hosek_pool.allocate(1);
        instance_collection->hosek_dummy_row->write_data(&zero);
    }

    // --- Binding set (explicit slots; MUST match the register() annotations
    // in volumetric_fog_composite.cs.slang) ---
    nvrhi::BindingSetDesc binding_set_desc;
    binding_set_desc.bindings = {
        // Light buffer (t6) + Hosek state buffer (t7)
        nvrhi::BindingSetItem::StructuredBuffer_SRV(
            6, instance_collection->light_pool.get_device_buffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(
            7, hosek_pool.get_device_buffer()),
        // Scene color (t10) + G-Buffer position (t11)
        nvrhi::BindingSetItem::Texture_SRV(10, scene_color),
        nvrhi::BindingSetItem::Texture_SRV(11, g_pos),
        // Constants + output
        nvrhi::BindingSetItem::ConstantBuffer(0, view_cb),
        nvrhi::BindingSetItem::ConstantBuffer(1, constants_cb),
        nvrhi::BindingSetItem::Texture_UAV(0, output),
    };
    auto binding_set =
        resource_allocator.create(binding_set_desc, binding_layout);
    MARK_DESTROY_NVRHI_RESOURCE(binding_set);

    // --- Dispatch ---
    auto command_list = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(command_list);

    command_list->open();
    // Transition inputs out of their previous pass's state: the deferred
    // output UAV and the rasterize G-Buffer render targets are both
    // keepInitialState resources needing explicit barriers, or the GPU reads
    // a stale state and the device is removed.
    command_list->setTextureState(
        scene_color,
        nvrhi::AllSubresources,
        nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(
        g_pos, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    command_list->setTextureState(
        output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    command_list->setBufferState(
        instance_collection->light_pool.get_device_buffer(),
        nvrhi::ResourceStates::ShaderResource);
    command_list->setBufferState(
        hosek_pool.get_device_buffer(), nvrhi::ResourceStates::ShaderResource);
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

NODE_DECLARATION_UI(volumetric_fog_composite);
NODE_DEF_CLOSE_SCOPE
