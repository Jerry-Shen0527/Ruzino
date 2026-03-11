
#include <pxr/base/gf/vec2i.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>

#include "../../../Core/RHI/source/shaderCompiler.h"
#include "../source/renderTLAS.h"
#include "GPUContext/program_vars.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "RHI/internal/resources.hpp"
#include "Scene/MaterialParamsBuffer.slang"
#include "camera.h"
#include "hd_RUZINO/render_node_base.h"
#include "light.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/shaders/utils/HitObject.h"

// A traditional path tracing node

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(path_tracing)
{
    b.add_input<nvrhi::BufferHandle>("Pixel Target");
    b.add_input<nvrhi::BufferHandle>("Rays");
    b.add_input<nvrhi::BufferHandle>("Random Seeds");
    b.add_input<bool>("Use Sampled Spectrum").default_val(false);

    b.add_output<nvrhi::TextureHandle>("Output");

    // Function content omitted
}

struct PathTracingStorage {
    constexpr static bool has_storage = false;
    GfVec2i old_size = GfVec2i(-1, -1);

    ProgramHandle path_tracing_program;
    std::unordered_map<unsigned, std::string> callable_shaders;
    ResourceAllocator* rc;

    nvrhi::TextureHandle output;

    nvrhi::BufferHandle material_params_buffer;
    nvrhi::BufferHandle pathTracingConstantsBuffer;

    nvrhi::SamplerHandle sampler;

    std::unique_ptr<ProgramVars> cached_program_vars;
    std::unique_ptr<RaytracingContext> cached_rt_context;

    // Dome light custom shader state
    std::string dome_light_shader_path;
    bool has_dome_light_shader = false;

    // Spectrum type state
    bool use_sampled_spectrum = false;

    // Custom shader materials: map from material_location to
    // eval_callable_index
    std::unordered_map<unsigned, unsigned> custom_shader_eval_indices;

    ~PathTracingStorage()
    {
        if (path_tracing_program && rc) {
            rc->destroy(path_tracing_program);
            path_tracing_program = nullptr;
        }
        if (material_params_buffer && rc) {
            rc->destroy(material_params_buffer);
            material_params_buffer = nullptr;
        }
        if (pathTracingConstantsBuffer && rc) {
            rc->destroy(pathTracingConstantsBuffer);
            pathTracingConstantsBuffer = nullptr;
        }
        if (sampler && rc) {
            rc->destroy(sampler);
            sampler = nullptr;
        }
    }
};

NODE_EXECUTION_FUNCTION(path_tracing)
{
    using namespace nvrhi;

    auto& g = global_payload;
    auto geom_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
    auto mat_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
    auto light_dirty =
        g.is_dirty(RenderGlobalPayload::SceneDirtyBits::DirtyLights);

    auto size = get_free_camera(params)->dataWindow.GetSize();
    auto& storage = params.get_storage<PathTracingStorage&>();
    bool size_changed = (storage.old_size != size);
    storage.old_size = size;

#ifdef _DEBUG
    if (storage.path_tracing_program &&
        storage.path_tracing_program->get_desc().check_shader_updated()) {
        mat_dirty = true;
    }
#endif

    if (geom_dirty || mat_dirty || light_dirty || size_changed)
        spdlog::info(
            "Path Tracing Node: geom_dirty={}, mat_dirty={}, light_dirty={}, "
            "size_changed={}",
            geom_dirty,
            mat_dirty,
            light_dirty,
            size_changed);

    storage.rc = &(resource_allocator);

    // Check for dome light with valid custom shader
    std::string current_dome_shader_path;
    bool found_dome_shader = false;
    int shader_dome_light_count = 0;
    auto& all_lights = global_payload.get_lights();

    for (auto* light : all_lights) {
        if (light &&
            light->GetLightType() == pxr::HdPrimTypeTokens->domeLight) {
            auto* dome_light = dynamic_cast<Hd_RUZINO_Dome_Light*>(light);
            if (dome_light && dome_light->HasValidShader()) {
                shader_dome_light_count++;
                if (!found_dome_shader) {
                    // Use the first valid shader dome light
                    found_dome_shader = true;
                    current_dome_shader_path = dome_light->GetShaderPath();
                }
            }
        }
    }

    // Warn if multiple shader dome lights found
    if (shader_dome_light_count > 1) {
        spdlog::warn(
            "Multiple dome lights with custom shaders found ({}), only using "
            "the first one!",
            shader_dome_light_count);
    }

    // Check if dome light shader changed
    bool dome_shader_changed =
        (found_dome_shader != storage.has_dome_light_shader) ||
        (found_dome_shader &&
         current_dome_shader_path != storage.dome_light_shader_path);

    // Check if Spectrum type changed
    bool use_sampled_spectrum = params.get_input<bool>("Use Sampled Spectrum");
    bool spectrum_type_changed =
        (use_sampled_spectrum != storage.use_sampled_spectrum);
    storage.use_sampled_spectrum = use_sampled_spectrum;

    if (spectrum_type_changed) {
        g.reset_accumulation = true;
    }

    if (mat_dirty || !storage.path_tracing_program || dome_shader_changed ||
        spectrum_type_changed) {
        if (!storage.path_tracing_program) {
            spdlog::info("Creating path tracing shader program");
        }
        ProgramDesc program_desc;
        program_desc.set_path("shaders/path_tracing.slang");
        program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;
#if 0 

        program_desc.nvapi_support = true;

        // Enable Shader Execution Reordering (SER) via NVAPI
        program_desc.hlslExtensionsUAV = 127;
        spdlog::info(
            "Enabling Shader Execution Reordering (SER) with NVAPI extension "
            "slot u127");
#endif

        // Define macro for spectrum type
        if (use_sampled_spectrum) {
            program_desc.define("USE_SAMPLED_SPECTRUM", "1");
            spdlog::info("Using Sampled Spectrum");
        }
        else {
            program_desc.define("USE_RGB_SPECTRUM", "1");
            spdlog::info("Using RGB Spectrum");
        }

        // Define macro for dome light custom shader
        if (found_dome_shader) {
            program_desc.define("USE_DOME_LIGHT_CALLABLE", "1");
            spdlog::info("Enabling dome light callable shader");
        }
        else {
            program_desc.define("USE_DOME_LIGHT_CALLABLE", "0");
        }

        // Add callable shader files
        program_desc.add_path("shaders/callables/eval_fallback.slang");
        program_desc.add_path("shaders/callables/eval_standard_surface.slang");
        program_desc.add_path("shaders/callables/eval_preview_surface.slang");

        // Add dome light custom shader if it exists
        if (found_dome_shader) {
            // Path already validated above
            std::filesystem::path shader_path(current_dome_shader_path);
            if (!shader_path.is_absolute()) {
                shader_path = std::filesystem::path(
                                  SlangShaderCompiler::get_shader_dir(
                                      ShaderDirType::Renderer)) /
                              current_dome_shader_path;
            }

            program_desc.add_path(shader_path.string());
            storage.dome_light_shader_path = current_dome_shader_path;
            storage.has_dome_light_shader = true;
            spdlog::info("Added dome light shader: {}", shader_path.string());
        }
        else {
            storage.has_dome_light_shader = false;
            storage.dome_light_shader_path.clear();
        }

        auto& materials = global_payload.get_materials();

        storage.callable_shaders.clear();
        storage.custom_shader_eval_indices.clear();

        // Track next available eval callable index (after standard ones: 0,1,2)
        int next_eval_index = 3;

        for (auto material : materials) {
            if (material.second == nullptr) {
                spdlog::warn(
                    "Null material found in path tracing node, {}",
                    material.first.GetText());
                return false;
                continue;
            }
            auto location = material.second->GetMaterialLocation();
            if (location == -1) {
                continue;
            }

            // Check if this is a custom shader material
            if (material.second->HasValidShader()) {
                // This is a custom eval callable - add shader file by path
                std::filesystem::path shader_path(
                    material.second->GetShaderPath());
                if (!shader_path.is_absolute()) {
                    shader_path = std::filesystem::path(
                                      SlangShaderCompiler::get_shader_dir(
                                          ShaderDirType::Renderer)) /
                                  material.second->GetShaderPath();
                }
                program_desc.add_path(shader_path.string());

                // Store the eval index for this material
                storage.custom_shader_eval_indices[location] = next_eval_index;
                storage.callable_shaders[location] =
                    material.second->GetMaterialName();

                spdlog::info(
                    "Material '{}': Custom eval shader '{}' at index {}",
                    material.first.GetText(),
                    shader_path.string(),
                    next_eval_index);

                // Generate fetch callable wrapper and opacity wrapper
                std::string fetch_wrapper =
                    R"(
import callable_data;
import Scene.BindlessMaterial;

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"((inout FetchCallableData data)
{
    // Custom shader material - no data to fetch, just set shader_type_id
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
}

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"(_opacity(inout FetchCallableData data)
{
    // Custom shader material - assume fully opaque
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
    data.material_params_index = asuint(1.0f);
}
)";
                program_desc.add_source_code(fetch_wrapper);

                next_eval_index++;
            }
            else {
                // Regular MaterialX material - use fetch+eval pattern
                program_desc.add_source_code(
                    material.second->GetShader(shader_factory));

                auto callable = material.second->GetShader(shader_factory);
                storage.callable_shaders[location] =
                    material.second->GetMaterialName();
            }
        }

        if (storage.path_tracing_program) {
            resource_allocator.destroy(storage.path_tracing_program);
        }
        storage.path_tracing_program = resource_allocator.create(program_desc);
        CHECK_PROGRAM_ERROR(storage.path_tracing_program);
    }

    if (size_changed || !storage.output)
        storage.output =
            create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);

    // Only material, light, or size changes require rebuilding program_vars and
    // pipeline Geometry changes only need TLAS update
    bool is_any_dirty = mat_dirty || light_dirty || size_changed;

    if (is_any_dirty || !storage.cached_program_vars ||
        !storage.cached_rt_context) {
        g.reset_accumulation = true;
        storage.cached_program_vars = std::make_unique<ProgramVars>(
            resource_allocator, storage.path_tracing_program);
        ProgramVars& program_vars = *storage.cached_program_vars;

        SamplerDesc sampler_desc;
        sampler_desc.addressU = nvrhi::SamplerAddressMode::Wrap;
        sampler_desc.addressV = nvrhi::SamplerAddressMode::Wrap;

        if (storage.sampler)
            resource_allocator.destroy(storage.sampler);
        storage.sampler = resource_allocator.create(sampler_desc);

        auto random_seeds =
            params.get_input<nvrhi::BufferHandle>("Random Seeds");

        program_vars["SceneBVH"] =
            params.get_global_payload<RenderGlobalPayload&>()
                .InstanceCollection->get_tlas();
        program_vars["inPixelTarget"] =
            params.get_input<nvrhi::BufferHandle>("Pixel Target");
        program_vars["output"] = storage.output;
        program_vars["random_seeds"] = random_seeds;
        for (int i = 0; i < 9; ++i) {
            program_vars["samplers"][i] = storage.sampler;
        }

        auto rays = params.get_input<nvrhi::BufferHandle>("Rays");
        program_vars["rays"] = rays;

        nvrhi::BufferDesc material_params_desc;
        // Each pixel should be able to store 288 bytes

        material_params_desc.byteSize =
            rays->getDesc().byteSize / sizeof(RayInfo) * sizeof(MaterialParams);
        material_params_desc.structStride = sizeof(MaterialParams);
        material_params_desc.canHaveUAVs = true;
        material_params_desc.initialState =
            nvrhi::ResourceStates::ShaderResource;
        material_params_desc.debugName = "materialParamsBuffer";
        material_params_desc.keepInitialState = true;
        if (storage.material_params_buffer)
            resource_allocator.destroy(storage.material_params_buffer);
        storage.material_params_buffer =
            resource_allocator.create(material_params_desc);

        //    program_vars["cb"] = create_constant_buffer(params, 1);

        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();

        program_vars["materialBlobBuffer"] =
            instance_collection->material_pool.get_device_buffer();
        program_vars["materialHeaderBuffer"] =
            instance_collection->material_header_pool.get_device_buffer();
        program_vars["materialParamsBuffer"] = storage.material_params_buffer;

        // Bind light buffer - only include lights with valid paths
        auto& all_lights = global_payload.get_lights();
        std::vector<Hd_RUZINO_Light*> valid_lights;

        for (auto* light : all_lights) {
            // Only include lights with non-empty paths (not fallback lights)
            if (light && !light->GetId().IsEmpty()) {
                valid_lights.push_back(light);
            }
        }

        uint32_t lightCount = static_cast<uint32_t>(valid_lights.size());

        instance_collection->light_pool.compress();
        program_vars["lightBuffer"] =
            instance_collection->light_pool.get_device_buffer();

        // Create unified path tracing constants buffer
        struct PathTracingConstants {
            uint32_t lightCount;
            uint32_t domeLightCallableIndex;
            uint32_t materialFetchCallableBaseIndex;
            uint32_t materialOpacityCallableOffset;
        };

        PathTracingConstants constants;
        constants.lightCount = lightCount;

        // Calculate indices based on custom shader materials
        int num_custom_evals = storage.custom_shader_eval_indices.size();

        // Dome light callable comes after custom material evals (index 3 +
        // num_custom_evals)
        constants.domeLightCallableIndex =
            storage.has_dome_light_shader ? (3 + num_custom_evals) : 0;

        // Material fetch callables come after dome light
        int num_materials = storage.callable_shaders.size();
        constants.materialFetchCallableBaseIndex =
            3 + num_custom_evals + (storage.has_dome_light_shader ? 1 : 0);

        // Opacity callables are placed right after fetch callables
        // Offset is the number of materials (each has a fetch callable)
        constants.materialOpacityCallableOffset = num_materials;

        if (storage.pathTracingConstantsBuffer)
            resource_allocator.destroy(storage.pathTracingConstantsBuffer);
        storage.pathTracingConstantsBuffer =
            create_constant_buffer(params, constants);
        program_vars["ptConstants"] = storage.pathTracingConstantsBuffer;

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

        storage.cached_rt_context = std::make_unique<RaytracingContext>(
            resource_allocator, program_vars);

        RaytracingContext& context = *storage.cached_rt_context;

        context.announce_raygeneration("RayGen");
        context.announce_hitgroup(
            "ClosestHit",
            "",
            "",
            0);  // Primary ray hit group at index 0 (triangles)
        context.announce_hitgroup(
            "ShadowHit",
            "",
            "",
            1);  // Shadow ray hit group at index 1 (triangles)
        context.announce_hitgroup(
            "SphereClosestHit",
            "",
            "SphereIntersection",
            2);  // Sphere primary ray with custom intersection
        context.announce_hitgroup(
            "SphereShadowHit",
            "",
            "SphereIntersection",
            3);  // Sphere shadow ray with custom intersection
        context.announce_miss("Miss", 0);  // Primary ray miss shader at index 0
        context.announce_miss(
            "ShadowMiss", 1);  // Shadow ray miss shader at index 1

        // Register shared material evaluation callables at fixed indices
        // Pass nullptr for local root signature since these callables use
        // global resources
        context.announce_callable(
            "eval_standard_surface",
            0,
            nullptr);  // shader_type_id = 0
        context.announce_callable(
            "eval_preview_surface", 1, nullptr);  // shader_type_id = 1
        context.announce_callable("eval_fallback", 2, nullptr);

        // Register custom shader eval callables starting from index 3
        int next_eval_index = 3;
        for (auto& entry : storage.custom_shader_eval_indices) {
            unsigned material_location = entry.first;
            unsigned eval_index = entry.second;
            std::string callable_name =
                "eval_" + storage.callable_shaders[material_location];
            context.announce_callable(callable_name, eval_index, nullptr);
            spdlog::info(
                "Registered custom eval callable '{}' at index {}",
                callable_name,
                eval_index);
        }

        // Register dome light custom callable after custom material evals
        if (storage.has_dome_light_shader) {
            // Extract callable name from shader path
            std::filesystem::path shader_path(storage.dome_light_shader_path);
            std::string callable_name = shader_path.stem().string();
            int dome_index =
                next_eval_index + storage.custom_shader_eval_indices.size();
            context.announce_callable(callable_name, dome_index, nullptr);
            spdlog::info(
                "Registered dome light callable '{}' at index {}",
                callable_name,
                dome_index);
        }

        // Register per-material data fetch callables
        int base_fetch_index = constants.materialFetchCallableBaseIndex;
        for (auto& callable : storage.callable_shaders) {
            std::string fetch_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second  // Custom shader fetch wrapper
                    : callable.second;            // MaterialX fetch callable
            context.announce_callable(
                fetch_name, base_fetch_index + callable.first, nullptr);
        }

        // Register per-material opacity fetch callables
        // These are placed after all fetch callables
        int base_opacity_index = base_fetch_index + num_materials;
        for (auto& callable : storage.callable_shaders) {
            std::string opacity_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second +
                          "_opacity"  // Custom shader opacity wrapper
                    : callable.second +
                          "_opacity";  // MaterialX opacity callable
            context.announce_callable(
                opacity_name, base_opacity_index + callable.first, nullptr);
            spdlog::info(
                "Registered opacity callable '{}' at index {}",
                opacity_name,
                base_opacity_index + callable.first);
        }

        context.finish_announcing_shader_names();
    }
    else if (geom_dirty) {
        // Geometry changed but no material/light/size change
        // Update geometry-related buffers in program_vars
        ProgramVars& program_vars = *storage.cached_program_vars;

        program_vars["SceneBVH"] =
            params.get_global_payload<RenderGlobalPayload&>()
                .InstanceCollection->get_tlas();
        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();

        program_vars.finish_setting_vars();

        // Reset accumulation
        g.reset_accumulation = true;
        spdlog::info(
            "Updated geometry buffers (TLAS, instanceDesc, meshDesc) for "
            "geometry change");
    }

    auto rays = params.get_input<nvrhi::BufferHandle>("Rays");
    auto buffer_size = rays->getDesc().byteSize / sizeof(RayInfo);

    if (buffer_size > 0) {
        storage.cached_rt_context->begin();
        storage.cached_rt_context->trace_rays(
            {}, *storage.cached_program_vars, buffer_size, 1, 1);
        storage.cached_rt_context->finish();
    }

    params.set_output("Output", storage.output);

    return true;
}

NODE_DECLARATION_UI(path_tracing);
NODE_DEF_CLOSE_SCOPE
