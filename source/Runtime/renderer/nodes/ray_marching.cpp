#include "../source/renderTLAS.h"
#include "GPUContext/raytracing_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "shaders/utils/HitObject.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(ray_marching)
{
    b.add_input<nvrhi::BufferHandle>("Pixel Target");
    b.add_input<nvrhi::BufferHandle>("Rays");
    b.add_input<nvrhi::BufferHandle>("Random Seeds");

    b.add_output<nvrhi::TextureHandle>("Output");
}

NODE_EXECUTION_FUNCTION(ray_marching)
{
    using namespace nvrhi;

    ProgramDesc program_desc;
    program_desc.set_path("sdf.slang");
    program_desc.shaderType = nvrhi::ShaderType::AllRayTracing;
    program_desc.nvapi_support = true;

    auto raytrace_compiled = resource_allocator.create(program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(raytrace_compiled);
    CHECK_PROGRAM_ERROR(raytrace_compiled);

    auto m_CommandList = resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(m_CommandList);

    auto output =
        create_default_render_target(params, nvrhi::Format::RGBA32_FLOAT);

    ProgramVars program_vars(resource_allocator, raytrace_compiled);

    SamplerDesc sampler_desc;
    sampler_desc.addressU = nvrhi::SamplerAddressMode::Wrap;
    sampler_desc.addressV = nvrhi::SamplerAddressMode::Wrap;

    auto sampler = resource_allocator.create(sampler_desc);
    MARK_DESTROY_NVRHI_RESOURCE(sampler);

    auto random_seeds = params.get_input<nvrhi::BufferHandle>("Random Seeds");

    program_vars["inPixelTarget"] =
        params.get_input<nvrhi::BufferHandle>("Pixel Target");
    program_vars["output"] = output;
    program_vars["random_seeds"] = random_seeds;
    program_vars["rays"] = params.get_input<nvrhi::BufferHandle>("Rays");

    // Bind the NanoVDB buffer from bindless data entry 0
    program_vars.set_descriptor_table(
        "t_BindlessBuffers",
        instance_collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        instance_collection->bindlessData.bufferBindlessLayout);

    program_vars.finish_setting_vars();

    RaytracingContext context(resource_allocator, program_vars);

    context.announce_raygeneration("RayGen");
    context.announce_miss("Miss");

    context.finish_announcing_shader_names();

    auto rays = params.get_input<nvrhi::BufferHandle>("Rays");
    auto buffer_size = rays->getDesc().byteSize / sizeof(RayInfo);

    if (buffer_size > 0) {
        context.begin();
        context.trace_rays({}, program_vars, buffer_size, 1, 1);
        context.finish();
    }

    params.set_output("Output", output);

    return true;
}

NODE_DECLARATION_UI(ray_marching);
NODE_DEF_CLOSE_SCOPE
