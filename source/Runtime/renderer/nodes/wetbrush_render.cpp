// wetbrush_render — a path-tracing render node that adds TWO extra hit groups
// (VolumeClosestHit / VolumeShadowHit with the VolumeIntersection procedural
// intersection shader) on top of the standard path_tracing node, so the
// Hd_RUZINO_Volume (WetbrushVolumeImpl) density-slab rprim can be raymarched
// by the path tracer.
//
// This node is a faithful copy of path_tracing.cpp's pipeline (program build,
// material-callable registration, SBT, bindings, dispatch) with the additions:
//   * program_desc also adds "volume_intersection.slang";
//   * the SBT registers two more hit groups at slots 4/5 routed to the volume
//     intersection/closest-hit/shadow shaders;
//   * program_vars binds the new volumeDescBuffer.
//
// socket contract is identical to path_tracing (Pixel Target / Rays / Random
// Seeds in, Output out) so it drops into the same render graph slot.
//
// path_tracing.cpp / path_tracing.slang are left untouched.
#include <pxr/base/gf/vec2i.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>

#include "../source/renderTLAS.h"
#include "GPUContext/program_vars.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "RHI/internal/resources.hpp"
#include "RHI/shaderCompiler.h"
#include "Scene/MaterialParamsBuffer.slang"
#include "camera.h"
#include "hd_RUZINO/render_node_base.h"
#include "light.h"
#include "material/material.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/utils/HitObject.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(wetbrush_render)
{
    b.add_input<nvrhi::BufferHandle>("Pixel Target");
    b.add_input<nvrhi::BufferHandle>("Rays");
    b.add_input<nvrhi::BufferHandle>("Random Seeds");
    b.add_input<bool>("Use Sampled Spectrum").default_val(false);

    b.add_output<nvrhi::TextureHandle>("Output");
}

struct WetbrushRenderStorage {
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

    std::string dome_light_shader_path;
    bool has_dome_light_shader = false;

    bool use_sampled_spectrum = false;

    std::unordered_map<unsigned, unsigned> custom_shader_eval_indices;

    ~WetbrushRenderStorage()
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

NODE_EXECUTION_FUNCTION(wetbrush_render)
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
    auto& storage = params.get_storage<WetbrushRenderStorage&>();
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
            "WetbrushRender Node: geom_dirty={}, mat_dirty={}, light_dirty={}, "
            "size_changed={}",
            geom_dirty,
            mat_dirty,
            light_dirty,
            size_changed);

    // Geometry content changed (e.g. the wetbrush volume rprim picked up a new
    // sim frame via the zero-copy registry). The path tracer must restart
    // accumulation from scratch, otherwise the accumulate node keeps averaging
    // new samples on top of the previous frame's buffer and painted regions
    // appear to darken/brighten over time. Material/light/size changes already
    // reset elsewhere; geometry changes did not.
    if (geom_dirty) {
        g.reset_accumulation = true;
    }

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
                    found_dome_shader = true;
                    current_dome_shader_path = dome_light->GetShaderPath();
                }
            }
        }
    }

    if (shader_dome_light_count > 1) {
        spdlog::warn(
            "Multiple dome lights with custom shaders found ({}), only using "
            "the first one!",
            shader_dome_light_count);
    }

    bool dome_shader_changed =
        (found_dome_shader != storage.has_dome_light_shader) ||
        (found_dome_shader &&
         current_dome_shader_path != storage.dome_light_shader_path);

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
            spdlog::info("Creating WetbrushRender shader program");
        }
        ProgramDesc program_desc;
        // Use wetbrush_render.slang (a copy of path_tracing.slang + the 3
        // volume hit-group entry points) so the volume entry points share the
        // same translation unit as RayPayload / ShadowPayload / the Scene
        // imports. path_tracing.slang is NOT modified;
        // volume_intersection.slang holds the importable helpers
        // (samplePaintField, intersectSlab, etc.).
        program_desc.set_path("wetbrush_render.slang");
        program_desc.add_path("volume_intersection.slang");
        program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;

        if (use_sampled_spectrum) {
            program_desc.define("USE_SAMPLED_SPECTRUM", "1");
        }
        else {
            program_desc.define("USE_RGB_SPECTRUM", "1");
        }

        if (found_dome_shader) {
            program_desc.define("USE_DOME_LIGHT_CALLABLE", "1");
        }
        else {
            program_desc.define("USE_DOME_LIGHT_CALLABLE", "0");
        }

        program_desc.add_path("callables/eval_fallback.slang");
        program_desc.add_path("callables/eval_standard_surface.slang");
        program_desc.add_path("callables/eval_preview_surface.slang");

        if (found_dome_shader) {
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
        }
        else {
            storage.has_dome_light_shader = false;
            storage.dome_light_shader_path.clear();
        }

        auto& materials = global_payload.get_materials();

        storage.callable_shaders.clear();
        storage.custom_shader_eval_indices.clear();

        int next_eval_index = 3;

        for (auto material : materials) {
            if (material.second == nullptr) {
                spdlog::warn(
                    "Null material found in wetbrush_render node, {}",
                    material.first.GetText());
                return false;
            }
            auto location = material.second->GetMaterialLocation();
            if (location == -1) {
                continue;
            }

            if (material.second->HasValidShader()) {
                std::filesystem::path shader_path(
                    material.second->GetShaderPath());
                if (!shader_path.is_absolute()) {
                    shader_path = std::filesystem::path(
                                      SlangShaderCompiler::get_shader_dir(
                                          ShaderDirType::Renderer)) /
                                  material.second->GetShaderPath();
                }
                program_desc.add_path(shader_path.string());

                storage.custom_shader_eval_indices[location] = next_eval_index;
                storage.callable_shaders[location] =
                    material.second->GetMaterialName();

                std::string fetch_wrapper =
                    R"(
import callable_data;
import Scene.BindlessMaterial;

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"((inout FetchCallableData data)
{
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
}

[shader("callable")]
void fetch_)" + material.second->GetMaterialName() +
                    R"(_opacity(inout FetchCallableData data)
{
    data.shader_type_id = )" +
                    std::to_string(next_eval_index) + R"(;
    data.material_params_index = asuint(1.0f);
    data.opacityColor = float3(1.0, 1.0, 1.0);
}
)";
                program_desc.add_source_code(fetch_wrapper);

                next_eval_index++;
            }
            else {
                auto shader_source = material.second->GetShader(shader_factory);
                auto mat_name = material.second->GetMaterialName();

                if (mat_name.empty() || shader_source.empty()) {
                    spdlog::warn(
                        "Material '{}': shader not ready (name='{}'), "
                        "skipping",
                        material.first.GetText(),
                        mat_name);
                    continue;
                }

                program_desc.add_source_code(shader_source);
                storage.callable_shaders[location] = mat_name;
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

        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();
        // Bind the volume descriptor buffer (the WetbrushVolume rprim's
        // VolumeDesc entries, indexed by GeometryInstanceData.geometryID).
        program_vars["volumeDescBuffer"] =
            instance_collection->volume_pool.get_device_buffer();

        program_vars["materialBlobBuffer"] =
            instance_collection->material_pool.get_device_buffer();
        program_vars["materialHeaderBuffer"] =
            instance_collection->material_header_pool.get_device_buffer();
        program_vars["materialParamsBuffer"] = storage.material_params_buffer;

        auto& all_lights = global_payload.get_lights();
        std::vector<Hd_RUZINO_Light*> valid_lights;
        for (auto* light : all_lights) {
            if (light && !light->GetId().IsEmpty()) {
                valid_lights.push_back(light);
            }
        }

        instance_collection->light_pool.compress();
        uint32_t lightCount =
            static_cast<uint32_t>(instance_collection->light_pool.count());

        program_vars["lightBuffer"] =
            instance_collection->light_pool.get_device_buffer();
        // hosekStateBuffer: see path_tracing.cpp — declared unconditionally in
        // pt_sample_lights.slang, must always be bound.
        program_vars["hosekStateBuffer"] =
            instance_collection->hosek_state_pool.get_device_buffer();

        struct PathTracingConstants {
            uint32_t lightCount;
            uint32_t domeLightCallableIndex;
            uint32_t materialFetchCallableBaseIndex;
            uint32_t materialOpacityCallableOffset;
        };

        PathTracingConstants constants;
        constants.lightCount = lightCount;

        int num_custom_evals = storage.custom_shader_eval_indices.size();

        constants.domeLightCallableIndex =
            storage.has_dome_light_shader ? (3 + num_custom_evals) : 0;

        int num_materials = storage.callable_shaders.size();
        constants.materialFetchCallableBaseIndex =
            3 + num_custom_evals + (storage.has_dome_light_shader ? 1 : 0);

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
        context.announce_hitgroup("ClosestHit", "", "", 0);
        context.announce_hitgroup("ShadowHit", "", "", 1);
        context.announce_hitgroup(
            "SphereClosestHit", "", "SphereIntersection", 2);
        context.announce_hitgroup(
            "SphereShadowHit", "", "SphereIntersection", 3);
        // Volume hit groups (slots 4/5) — the addition over path_tracing.
        // Routed only to instances whose instanceContributionToHitGroupIndex
        // is 4 (a WetbrushVolumeImpl via Hd_RUZINO_Volume).
        context.announce_hitgroup(
            "VolumeClosestHit", "", "VolumeIntersection", 4);
        context.announce_hitgroup(
            "VolumeShadowHit", "", "VolumeIntersection", 5);
        context.announce_miss("Miss", 0);
        context.announce_miss("ShadowMiss", 1);

        context.announce_callable("eval_standard_surface", 0, nullptr);
        context.announce_callable("eval_preview_surface", 1, nullptr);
        context.announce_callable("eval_fallback", 2, nullptr);

        int next_eval_index = 3;
        for (auto& entry : storage.custom_shader_eval_indices) {
            unsigned material_location = entry.first;
            unsigned eval_index = entry.second;
            std::string callable_name =
                "eval_" + storage.callable_shaders[material_location];
            context.announce_callable(callable_name, eval_index, nullptr);
        }

        if (storage.has_dome_light_shader) {
            std::filesystem::path shader_path(storage.dome_light_shader_path);
            std::string callable_name = shader_path.stem().string();
            int dome_index =
                next_eval_index + storage.custom_shader_eval_indices.size();
            context.announce_callable(callable_name, dome_index, nullptr);
        }

        int base_fetch_index = constants.materialFetchCallableBaseIndex;
        for (auto& callable : storage.callable_shaders) {
            std::string fetch_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second
                    : callable.second;
            context.announce_callable(
                fetch_name, base_fetch_index + callable.first, nullptr);
        }

        int base_opacity_index = base_fetch_index + num_materials;
        for (auto& callable : storage.callable_shaders) {
            std::string opacity_name =
                storage.custom_shader_eval_indices.count(callable.first) > 0
                    ? "fetch_" + callable.second + "_opacity"
                    : callable.second + "_opacity";
            context.announce_callable(
                opacity_name, base_opacity_index + callable.first, nullptr);
        }

        context.finish_announcing_shader_names();
    }
    else if (geom_dirty) {
        ProgramVars& program_vars = *storage.cached_program_vars;

        program_vars["SceneBVH"] =
            params.get_global_payload<RenderGlobalPayload&>()
                .InstanceCollection->get_tlas();
        program_vars["instanceDescBuffer"] =
            instance_collection->instance_pool.get_device_buffer();
        program_vars["meshDescBuffer"] =
            instance_collection->mesh_pool.get_device_buffer();
        program_vars["volumeDescBuffer"] =
            instance_collection->volume_pool.get_device_buffer();

        program_vars.finish_setting_vars();

        g.reset_accumulation = true;
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

NODE_DECLARATION_UI(wetbrush_render);

NODE_DEF_CLOSE_SCOPE
