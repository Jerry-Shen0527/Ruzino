
#include "../source/renderTLAS.h"
#include "GPUContext/raytracing_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "nvrhi/utils.h"
#include "shaders/utils/HitObject.h"
#include "spdlog/spdlog.h"
#include "utils/math.h"
NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(material_eval_sample_pdf)
{
    b.add_input<nvrhi::BufferHandle>("PixelTarget");
    b.add_input<nvrhi::BufferHandle>("HitInfo");
    b.add_input<nvrhi::BufferHandle>("Random Seeds");

    b.add_output<nvrhi::BufferHandle>("PixelTarget");
    b.add_input<int>("Buffer Size").min(1).max(10).default_val(4);
    b.add_output<nvrhi::BufferHandle>("Eval");
    b.add_output<nvrhi::BufferHandle>("Sample");
    b.add_output<nvrhi::BufferHandle>("Weight");
    b.add_output<nvrhi::BufferHandle>("Pdf");
}

struct EnvironmentPrefilterData {
    float4x4 u_envMatrix;
    float3 u_envLightIntensity;
    int u_envRadianceMips;
    int u_envIrradianceMips;
};

NODE_EXECUTION_FUNCTION(material_eval_sample_pdf)
{
    using namespace nvrhi;

    ProgramDesc program_desc;
    program_desc.set_path("material_eval_sample_pdf.slang");
    program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;
    program_desc.nvapi_support = true;

    auto& materials = global_payload.get_materials();

    std::unordered_map<unsigned, std::string> callable_shaders;

    for (auto material : materials) {
        auto location = material.second->GetMaterialLocation();
        if (location == -1) {
            continue;
        }

        program_desc.add_source_code(
            material.second->GetShader(shader_factory));

        auto callable = material.second->GetShader(shader_factory);
        callable_shaders[location] = material.second->GetMaterialName();

        // combined_desc.add_component(callable->get_linked_program());
    }

    auto raytrace_compiled = resource_allocator.create(program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(raytrace_compiled);
    CHECK_PROGRAM_ERROR(raytrace_compiled);

    auto m_CommandList = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(m_CommandList);

    // 0. Get the 'HitObjectInfos'

    auto hit_info_buffer = params.get_input<BufferHandle>("HitInfo");
    auto in_pixel_target_buffer = params.get_input<BufferHandle>("PixelTarget");

    auto length = hit_info_buffer->getDesc().byteSize / sizeof(HitObjectInfo);

    length = std::max(length, static_cast<decltype(length)>(1));

    // The Eval, Pixel Target together should be the same size, and should
    // together be able to store the result of the material evaluation

    auto buffer_desc = BufferDesc{}
                           .setByteSize(length * sizeof(pxr::GfVec2i))
                           .setStructStride(sizeof(pxr::GfVec2i))
                           .setKeepInitialState(true)
                           .setInitialState(ResourceStates::UnorderedAccess)
                           .setCanHaveUAVs(true);
    auto pixel_target_buffer = resource_allocator.create(buffer_desc);

    buffer_desc.setByteSize(length * sizeof(pxr::GfVec4f))
        .setStructStride(sizeof(pxr::GfVec4f));
    auto eval_buffer = resource_allocator.create(buffer_desc);

    buffer_desc.setByteSize(length * sizeof(RayInfo))
        .setStructStride(sizeof(RayInfo));
    auto sample_buffer = resource_allocator.create(buffer_desc);

    buffer_desc.setByteSize(length * sizeof(float))
        .setStructStride(sizeof(float));
    auto weight_buffer = resource_allocator.create(buffer_desc);

    // 'Pdf Should be just like float...'
    buffer_desc.setByteSize(length * sizeof(float))
        .setStructStride(sizeof(float));
    auto pdf_buffer = resource_allocator.create(buffer_desc);

    auto random_seeds = params.get_input<BufferHandle>("Random Seeds");
    // Set the program variables

    SamplerDesc sampler_desc;
    sampler_desc.addressU = nvrhi::SamplerAddressMode::Wrap;
    sampler_desc.addressV = nvrhi::SamplerAddressMode::Wrap;

    auto sampler = resource_allocator.create(sampler_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sampler);

    ProgramVars program_vars(resource_allocator, raytrace_compiled);
    program_vars["SceneBVH"] = params.get_global_payload<RenderGlobalPayload&>()
                                   .InstanceCollection->get_tlas();
    program_vars["hitObjects"] = hit_info_buffer;
    program_vars["in_PixelTarget"] = in_pixel_target_buffer;
    program_vars["PixelTarget"] = pixel_target_buffer;
    program_vars["Eval"] = eval_buffer;
    program_vars["Sample"] = sample_buffer;
    program_vars["Weight"] = weight_buffer;
    program_vars["Pdf"] = pdf_buffer;
    program_vars["random_seeds"] = random_seeds;
    program_vars["sampler"] = sampler;

    auto env_prefilter_data = EnvironmentPrefilterData{};
    auto env_prefilter_cb = create_constant_buffer(params, env_prefilter_data);
    MARK_DESTROY_NVRHI_RESOURCE(env_prefilter_cb);
    program_vars["u_envPrefilterData"] = env_prefilter_cb;

    auto u_envRadiance = create_empty_texture(params, { 4, 4 });
    auto u_envIrradiance = create_empty_texture(params, { 4, 4 });
    MARK_DESTROY_NVRHI_RESOURCE(u_envRadiance);
    MARK_DESTROY_NVRHI_RESOURCE(u_envIrradiance);
    program_vars["u_envRadiance"] = u_envRadiance;
    program_vars["u_envIrradiance"] = u_envIrradiance;

    auto refraction_twosided = create_constant_buffer(params, 0u);
    MARK_DESTROY_NVRHI_RESOURCE(refraction_twosided);
    program_vars["u_refractionTwoSided"] = refraction_twosided;

    program_vars["u_envRadiance_sampler"] = sampler;
    program_vars["u_envIrradiance_sampler"] = sampler;

    //    program_vars["cb"] = create_constant_buffer(params, 1);

    program_vars["instanceDescBuffer"] =
        instance_collection->instance_pool.get_device_buffer();
    program_vars["meshDescBuffer"] =
        instance_collection->mesh_pool.get_device_buffer();

    program_vars["materialBlobBuffer"] =
        instance_collection->material_pool.get_device_buffer();
    program_vars["materialHeaderBuffer"] =
        instance_collection->material_header_pool.get_device_buffer();

    program_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    program_vars.set_descriptor_table(
        "t_BindlessTextures",
        instance_collection->bindlessData.textureDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.textureBindlessLayout);
    program_vars.finish_setting_vars();

    RaytracingContext context(resource_allocator, program_vars);

    context.announce_raygeneration("RayGen");
    context.announce_hitgroup("ClosestHit");
    context.announce_hitgroup("ShadowHit", "", "", 1);
    context.announce_miss("Miss");
    context.announce_miss("ShadowMiss", 1);

    for (auto& callable : callable_shaders) {
        context.announce_callable(callable.second, callable.first);
    }

    context.finish_announcing_shader_names();

    // 2. Prepare the shader

    auto buffer_size = params.get_input<int>("Buffer Size");

    if (buffer_size > 0) {
        context.begin();
        context.trace_rays({}, program_vars, buffer_size, 1, 1);
        context.finish();
    }

    // 4. Get the result
    params.set_output("PixelTarget", pixel_target_buffer);
    params.set_output("Eval", eval_buffer);
    params.set_output("Sample", sample_buffer);
    params.set_output("Weight", weight_buffer);
    params.set_output("Pdf", pdf_buffer);
    return true;
}

NODE_DECLARATION_UI(material_eval_sample_pdf);
NODE_DEF_CLOSE_SCOPE
