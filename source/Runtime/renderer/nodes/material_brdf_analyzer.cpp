#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include "material/material.h"

using namespace pxr;

#include <spdlog/spdlog.h>

#include "../source/renderTLAS.h"
#include "GPUContext/compute_context.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "nvrhi/utils.h"
#include "utils/math.h"

// Material BRDF Analyzer Node
// Visualizes BRDF eval, pdf, and sample distribution for a given material

NODE_DEF_OPEN_SCOPE

// Environment prefilter data structure (from path_tracing)
struct EnvironmentPrefilterData {
    float4x4 u_envMatrix;
    float3 u_envLightIntensity;
    int u_envRadianceMips;
    int u_envIrradianceMips;
};

// Parameters for BRDF analysis
struct BRDFAnalysisParams {
    pxr::GfVec3f incident_direction;  // Wi in world space
    pxr::GfVec2f uv_coord;            // UV coordinate
    unsigned material_id;             // Material to analyze
    unsigned resolution;              // Output texture resolution
    unsigned num_samples;             // Number of samples for Monte Carlo
};

NODE_DECLARATION_FUNCTION(material_brdf_analyzer)
{
    b.add_input<float>("Incident Direction X").default_val(0.0f);
    b.add_input<float>("Incident Direction Y").default_val(0.0f);
    b.add_input<float>("Incident Direction Z").default_val(1.0f);
    b.add_input<float>("UV X").default_val(0.5f);
    b.add_input<float>("UV Y").default_val(0.5f);
    b.add_input<int>("Material ID").default_val(0).min(0).max(100);
    b.add_input<int>("Resolution").min(32).max(2048).default_val(512);
    b.add_input<int>("Num Samples").min(100).max(1000000).default_val(10000);

    b.add_output<nvrhi::TextureHandle>("BRDF Eval");
    b.add_output<nvrhi::TextureHandle>("PDF");
    b.add_output<nvrhi::TextureHandle>("Sample Distribution");
    b.add_output<nvrhi::TextureHandle>("Importance Test");
}

NODE_EXECUTION_FUNCTION(material_brdf_analyzer)
{
    using namespace nvrhi;

    // Get input parameters
    float incident_x = params.get_input<float>("Incident Direction X");
    float incident_y = params.get_input<float>("Incident Direction Y");
    float incident_z = params.get_input<float>("Incident Direction Z");
    float uv_x = params.get_input<float>("UV X");
    float uv_y = params.get_input<float>("UV Y");
    int material_id = params.get_input<int>("Material ID");
    int resolution = params.get_input<int>("Resolution");
    int num_samples = params.get_input<int>("Num Samples");

    // Normalize incident direction
    float3 incident_dir = float3(incident_x, incident_y, incident_z);
    float length = std::sqrt(
        incident_dir.x * incident_dir.x + incident_dir.y * incident_dir.y +
        incident_dir.z * incident_dir.z);
    if (length > 0.0f) {
        incident_dir.x /= length;
        incident_dir.y /= length;
        incident_dir.z /= length;
    }

    // Create output textures
    TextureDesc tex_desc;
    tex_desc.width = resolution;
    tex_desc.height = resolution;
    tex_desc.format = Format::RGBA32_FLOAT;
    tex_desc.isUAV = true;
    tex_desc.debugName = "BRDF_Eval_Output";
    tex_desc.initialState = ResourceStates::UnorderedAccess;
    tex_desc.keepInitialState = true;

    auto eval_output = resource_allocator.create(tex_desc);
    MARK_DESTROY_NVRHI_RESOURCE(eval_output);

    tex_desc.debugName = "PDF_Output";
    auto pdf_output = resource_allocator.create(tex_desc);
    MARK_DESTROY_NVRHI_RESOURCE(pdf_output);

    tex_desc.debugName = "Sample_Distribution";
    auto sample_output = resource_allocator.create(tex_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sample_output);

    // Get materials and setup callable shaders
    auto& materials = global_payload.get_materials();
    std::unordered_map<unsigned, std::string> callable_shaders;

    for (auto material : materials) {
        auto location = material.second->GetMaterialLocation();
        if (location == -1) {
            continue;
        }
        callable_shaders[location] = material.second->GetMaterialName();
    }

    // ===== BRDF Eval Shader =====
    ProgramDesc eval_program_desc;
    eval_program_desc.set_path("material_brdf_eval.slang");
    eval_program_desc.shaderType = ShaderType::AllRayTracing;
    eval_program_desc.nvapi_support = true;

    for (auto material : materials) {
        auto location = material.second->GetMaterialLocation();
        if (location == -1) {
            continue;
        }
        eval_program_desc.add_source_code(
            material.second->GetShader(shader_factory));
    }

    auto eval_compiled = resource_allocator.create(eval_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(eval_compiled);
    CHECK_PROGRAM_ERROR(eval_compiled);

    // Setup eval shader parameters
    struct AnalysisConstants {
        float3 incident_direction;
        float _pad0;
        float2 uv_coord;
        uint32_t material_id;
        uint32_t resolution;
    };

    AnalysisConstants analysis_constants;
    analysis_constants.incident_direction = incident_dir;
    analysis_constants.uv_coord = float2(uv_x, uv_y);
    analysis_constants.material_id = static_cast<uint32_t>(material_id);
    analysis_constants.resolution = static_cast<uint32_t>(resolution);

    auto analysis_cb = create_constant_buffer(params, analysis_constants);
    MARK_DESTROY_NVRHI_RESOURCE(analysis_cb);

    // Create sampler
    SamplerDesc sampler_desc;
    sampler_desc.addressU = SamplerAddressMode::Wrap;
    sampler_desc.addressV = SamplerAddressMode::Wrap;
    sampler_desc.addressW = SamplerAddressMode::Wrap;
    auto sampler = resource_allocator.create(sampler_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sampler);

    // Setup program vars for eval shader
    ProgramVars eval_vars(resource_allocator, eval_compiled);

    eval_vars["SceneBVH"] = instance_collection->get_tlas();
    eval_vars["eval_output"] = eval_output;
    eval_vars["pdf_output"] = pdf_output;
    eval_vars["analysis_params"] = analysis_cb;

    for (int i = 0; i < 9; ++i) {
        eval_vars["samplers"][i] = sampler;
    }

    // Bind MaterialX environment samplers (from .slang)
    eval_vars["u_envRadiance_sampler"] = sampler;
    eval_vars["u_envIrradiance_sampler"] = sampler;

    eval_vars["instanceDescBuffer"] =
        instance_collection->instance_pool.get_device_buffer();
    eval_vars["meshDescBuffer"] =
        instance_collection->mesh_pool.get_device_buffer();
    eval_vars["materialBlobBuffer"] =
        instance_collection->material_pool.get_device_buffer();
    eval_vars["materialHeaderBuffer"] =
        instance_collection->material_header_pool.get_device_buffer();

    eval_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    eval_vars.set_descriptor_table(
        "t_BindlessTextures",
        instance_collection->bindlessData.textureDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.textureBindlessLayout);

    eval_vars.finish_setting_vars();

    // Create raytracing context for eval shader
    RaytracingContext eval_context(resource_allocator, eval_vars);
    eval_context.announce_raygeneration("EvalPdfRayGen");
    eval_context.announce_hitgroup("EvalHit", "", "", 0);
    eval_context.announce_miss("EvalMiss", 0);

    for (auto& callable : callable_shaders) {
        eval_context.announce_callable(callable.second, callable.first);
    }

    eval_context.finish_announcing_shader_names();

    // Execute eval shader
    eval_context.begin();
    eval_context.trace_rays({}, eval_vars, resolution, resolution, 1);
    eval_context.finish();

    // ===== BRDF Sample Shader =====
    ProgramDesc sample_program_desc;
    sample_program_desc.set_path("material_brdf_sample.slang");
    sample_program_desc.shaderType = ShaderType::AllRayTracing;
    sample_program_desc.nvapi_support = true;

    for (auto material : materials) {
        auto location = material.second->GetMaterialLocation();
        if (location == -1) {
            continue;
        }
        sample_program_desc.add_source_code(
            material.second->GetShader(shader_factory));
    }

    auto sample_compiled = resource_allocator.create(sample_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sample_compiled);
    CHECK_PROGRAM_ERROR(sample_compiled);

    // Setup sample shader parameters
    struct SampleConstants {
        float3 incident_direction;
        float _pad0;
        float2 uv_coord;
        uint32_t material_id;
        uint32_t resolution;
        uint32_t num_samples;
    };

    SampleConstants sample_constants;
    sample_constants.incident_direction = incident_dir;
    sample_constants.uv_coord = float2(uv_x, uv_y);
    sample_constants.material_id = static_cast<uint32_t>(material_id);
    sample_constants.resolution = static_cast<uint32_t>(resolution);
    sample_constants.num_samples = static_cast<uint32_t>(num_samples);

    auto sample_cb = create_constant_buffer(params, sample_constants);
    MARK_DESTROY_NVRHI_RESOURCE(sample_cb);

    // Create sample accumulation buffer
    // Each pixel has 2 uints: [pdf_accumulator, sample_count] (float values
    // stored as uint)
    BufferDesc sample_buffer_desc;
    sample_buffer_desc.byteSize =
        sizeof(uint32_t) * resolution * resolution * 2;
    sample_buffer_desc.structStride = sizeof(uint32_t);
    sample_buffer_desc.canHaveUAVs = true;
    sample_buffer_desc.debugName = "Sample_Accumulation_Buffer";
    sample_buffer_desc.initialState = ResourceStates::UnorderedAccess;
    sample_buffer_desc.keepInitialState = true;

    auto sample_buffer = resource_allocator.create(sample_buffer_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sample_buffer);

    // Clear the buffer to zero
    auto cmd_list = RHI::get_device()->createCommandList();
    cmd_list->open();
    cmd_list->clearBufferUInt(
        sample_buffer, 0);  // Still works for float buffers (sets to 0.0)
    cmd_list->close();
    RHI::get_device()->executeCommandList(cmd_list.Get());
    RHI::get_device()->waitForIdle();

    // Setup program vars for sample shader
    ProgramVars sample_vars(resource_allocator, sample_compiled);
    sample_vars["SceneBVH"] = instance_collection->get_tlas();
    sample_vars["sample_buffer"] = sample_buffer;
    sample_vars["sample_params"] = sample_cb;

    for (int i = 0; i < 9; ++i) {
        sample_vars["samplers"][i] = sampler;
    }

    // Bind MaterialX environment samplers
    sample_vars["u_envRadiance_sampler"] = sampler;
    sample_vars["u_envIrradiance_sampler"] = sampler;

    sample_vars["instanceDescBuffer"] =
        instance_collection->instance_pool.get_device_buffer();
    sample_vars["meshDescBuffer"] =
        instance_collection->mesh_pool.get_device_buffer();
    sample_vars["materialBlobBuffer"] =
        instance_collection->material_pool.get_device_buffer();
    sample_vars["materialHeaderBuffer"] =
        instance_collection->material_header_pool.get_device_buffer();

    sample_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    sample_vars.set_descriptor_table(
        "t_BindlessTextures",
        instance_collection->bindlessData.textureDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.textureBindlessLayout);

    sample_vars.finish_setting_vars();

    // Create raytracing context for sample shader
    RaytracingContext sample_context(resource_allocator, sample_vars);
    sample_context.announce_raygeneration("SampleRayGen");
    sample_context.announce_hitgroup("SampleHit", "", "", 0);
    sample_context.announce_miss("SampleMiss", 0);

    for (auto& callable : callable_shaders) {
        sample_context.announce_callable(callable.second, callable.first);
    }

    sample_context.finish_announcing_shader_names();

    // Execute sample shader - dispatch num_samples threads (one per sample)
    // Note: D3D12/Vulkan DispatchRays has max 65535 per dimension
    // So we need to split into 2D dispatch
    uint32_t samples_per_dim = static_cast<uint32_t>(
        std::ceil(std::sqrt(static_cast<float>(num_samples))));
    spdlog::info(
        "Dispatching sample shader: {}x{} = {} samples",
        samples_per_dim,
        samples_per_dim,
        samples_per_dim * samples_per_dim);
    sample_context.begin();
    sample_context.trace_rays(
        {}, sample_vars, samples_per_dim, samples_per_dim, 1);
    sample_context.finish();

    // ===== Normalize Sample Buffer to Texture =====
    ProgramDesc normalize_program_desc;
    normalize_program_desc.set_path("material_brdf_normalize.slang");
    normalize_program_desc.shaderType = ShaderType::Compute;

    auto normalize_compiled = resource_allocator.create(normalize_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(normalize_compiled);
    CHECK_PROGRAM_ERROR(normalize_compiled);

    struct NormalizeConstants {
        uint32_t resolution;
        uint32_t num_samples;
        uint32_t min_samples_threshold;  // Minimum samples required for
                                         // importance test (0 = no filtering)
        uint32_t _pad;
    };

    NormalizeConstants normalize_constants;
    normalize_constants.resolution = static_cast<uint32_t>(resolution);
    normalize_constants.num_samples = static_cast<uint32_t>(num_samples);
    normalize_constants.min_samples_threshold =
        0;  // No filtering for sample distribution
    normalize_constants._pad = 0;

    auto normalize_cb = create_constant_buffer(params, normalize_constants);
    MARK_DESTROY_NVRHI_RESOURCE(normalize_cb);

    ProgramVars normalize_vars(resource_allocator, normalize_compiled);
    normalize_vars["sample_buffer"] = sample_buffer;
    normalize_vars["sample_output"] = sample_output;
    normalize_vars["normalize_params"] = normalize_cb;
    normalize_vars.finish_setting_vars();

    // Execute normalize compute shader using ComputeContext
    ComputeContext normalize_context(resource_allocator, normalize_vars);
    normalize_context.finish_setting_pso();

    normalize_context.begin();
    normalize_context.dispatch(
        {},  // No indirect params
        normalize_vars,
        resolution,
        16,  // width, groupSizeX
        resolution,
        16,  // height, groupSizeY
        1,
        1);  // depth, groupSizeZ
    normalize_context.finish();

    // ===== Importance Sampling Test =====
    // Sample from BRDF and divide by PDF to check uniformity
    ProgramDesc importance_program_desc;
    importance_program_desc.set_path(
        "material_brdf_importance_sample.slang");
    importance_program_desc.shaderType = ShaderType::AllRayTracing;
    importance_program_desc.nvapi_support = true;

    for (auto material : materials) {
        auto location = material.second->GetMaterialLocation();
        if (location == -1) {
            continue;
        }
        importance_program_desc.add_source_code(
            material.second->GetShader(shader_factory));
    }

    auto importance_compiled =
        resource_allocator.create(importance_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(importance_compiled);
    CHECK_PROGRAM_ERROR(importance_compiled);

    // Create importance test output texture
    tex_desc.debugName = "Importance_Test_Output";
    auto importance_output = resource_allocator.create(tex_desc);
    MARK_DESTROY_NVRHI_RESOURCE(importance_output);

    // Create importance accumulation buffer
    BufferDesc importance_buffer_desc;
    importance_buffer_desc.byteSize =
        sizeof(uint32_t) * resolution * resolution * 2;
    importance_buffer_desc.structStride = sizeof(uint32_t);
    importance_buffer_desc.canHaveUAVs = true;
    importance_buffer_desc.debugName = "Importance_Accumulation_Buffer";
    importance_buffer_desc.initialState = ResourceStates::UnorderedAccess;
    importance_buffer_desc.keepInitialState = true;

    auto importance_buffer = resource_allocator.create(importance_buffer_desc);
    MARK_DESTROY_NVRHI_RESOURCE(importance_buffer);

    // Clear the buffer
    auto cmd_list2 = RHI::get_device()->createCommandList();
    cmd_list2->open();
    cmd_list2->clearBufferUInt(importance_buffer, 0);
    cmd_list2->close();
    RHI::get_device()->executeCommandList(cmd_list2.Get());
    RHI::get_device()->waitForIdle();

    // Setup importance test parameters
    auto importance_cb = create_constant_buffer(params, sample_constants);
    MARK_DESTROY_NVRHI_RESOURCE(importance_cb);

    // Setup program vars
    ProgramVars importance_vars(resource_allocator, importance_compiled);
    importance_vars["SceneBVH"] = instance_collection->get_tlas();
    importance_vars["importance_buffer"] = importance_buffer;
    importance_vars["importance_params"] = importance_cb;

    for (int i = 0; i < 9; ++i) {
        importance_vars["samplers"][i] = sampler;
    }

    importance_vars["u_envRadiance_sampler"] = sampler;
    importance_vars["u_envIrradiance_sampler"] = sampler;

    importance_vars["instanceDescBuffer"] =
        instance_collection->instance_pool.get_device_buffer();
    importance_vars["meshDescBuffer"] =
        instance_collection->mesh_pool.get_device_buffer();
    importance_vars["materialBlobBuffer"] =
        instance_collection->material_pool.get_device_buffer();
    importance_vars["materialHeaderBuffer"] =
        instance_collection->material_header_pool.get_device_buffer();

    importance_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    importance_vars.set_descriptor_table(
        "t_BindlessTextures",
        instance_collection->bindlessData.textureDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.textureBindlessLayout);

    importance_vars.finish_setting_vars();

    // Create raytracing context
    RaytracingContext importance_context(resource_allocator, importance_vars);
    importance_context.announce_raygeneration("ImportanceSampleRayGen");
    importance_context.announce_hitgroup("ImportanceHit", "", "", 0);
    importance_context.announce_miss("ImportanceMiss", 0);

    for (auto& callable : callable_shaders) {
        importance_context.announce_callable(callable.second, callable.first);
    }

    importance_context.finish_announcing_shader_names();

    // Execute importance sampling
    spdlog::info(
        "Dispatching importance shader: {}x{} = {} samples",
        samples_per_dim,
        samples_per_dim,
        samples_per_dim * samples_per_dim);
    importance_context.begin();
    importance_context.trace_rays(
        {}, importance_vars, samples_per_dim, samples_per_dim, 1);
    importance_context.finish();

    // Normalize importance test results
    // Create separate constant buffer with threshold for importance test
    NormalizeConstants importance_normalize_constants;
    importance_normalize_constants.resolution =
        static_cast<uint32_t>(resolution);
    importance_normalize_constants.num_samples =
        static_cast<uint32_t>(num_samples);
    importance_normalize_constants.min_samples_threshold =
        8;  // Filter pixels with < 10 samples
    importance_normalize_constants._pad = 0;

    auto importance_normalize_cb =
        create_constant_buffer(params, importance_normalize_constants);
    MARK_DESTROY_NVRHI_RESOURCE(importance_normalize_cb);

    ProgramVars importance_normalize_vars(
        resource_allocator, normalize_compiled);
    importance_normalize_vars["sample_buffer"] = importance_buffer;
    importance_normalize_vars["sample_output"] = importance_output;
    importance_normalize_vars["normalize_params"] = importance_normalize_cb;
    importance_normalize_vars.finish_setting_vars();

    ComputeContext importance_normalize_context(
        resource_allocator, importance_normalize_vars);
    importance_normalize_context.finish_setting_pso();

    importance_normalize_context.begin();
    importance_normalize_context.dispatch(
        {}, importance_normalize_vars, resolution, 16, resolution, 16, 1, 1);
    importance_normalize_context.finish();

    // Set outputs
    params.set_output("BRDF Eval", eval_output);
    params.set_output("PDF", pdf_output);
    params.set_output("Sample Distribution", sample_output);
    params.set_output("Importance Test", importance_output);

    return true;
}

NODE_DECLARATION_REQUIRED(material_brdf_analyzer)
NODE_DECLARATION_UI(material_brdf_analyzer);
NODE_DEF_CLOSE_SCOPE
