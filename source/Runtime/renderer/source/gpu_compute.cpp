#include "gpu_compute.h"

#include <spdlog/spdlog.h>

#include "RHI/shaderCompiler.h"
#include "GPUContext/compute_context.hpp"
#include "GPUContext/program_vars.hpp"
#include "RHI/internal/resources.hpp"
#include "RHI/rhi.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/vec3f.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

// Define the macro locally if not available
template<typename T>
class RAII_resource_cleaner_local {
   public:
    RAII_resource_cleaner_local(ResourceAllocator& allocator)
        : allocator_(allocator)
    {
    }
    T set_data(T handle)
    {
        data = handle;
        return data;
    }
    ~RAII_resource_cleaner_local()
    {
        allocator_.destroy(data);
    }

   private:
    ResourceAllocator& allocator_;
    T data;
};
#define MARK_DESTROY_NVRHI_RESOURCE(resource)                           \
    RAII_resource_cleaner_local<decltype(resource)> resource##_cleaner( \
        get_instance().sa_resource_allocator);                          \
    resource##_cleaner.set_data(resource)

GPUSceneAssember GPUSceneAssember::instance;

void GPUSceneAssember::fill_instances(
    const pxr::GfMatrix4f& parent_transform,
    const pxr::VtIntArray& instance_indices,
    const pxr::GfVec3f* translations,
    const pxr::GfQuatf* rotations,
    const pxr::GfVec3f* scales,
    const pxr::GfMatrix4d* instanceTransforms,
    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle
        geometry_instance_buffer,
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle rt_instance_buffer,
    uint64_t BLAS_address,
    const pxr::GfMatrix4f& prototype_transform,
    unsigned material_id,
    unsigned geometry_id)
{
    std::lock_guard lock(execution_launch_mutex);

    if (instance_indices.empty()) {
        spdlog::warn(
            "GPUSceneAssember::fill_instances called with empty "
            "instance_indices");
        return;
    }

    spdlog::info(
        "GPUSceneAssember::fill_instances: instanceCount={}, geometryID={}, "
        "materialID={}",
        instance_indices.size(),
        geometry_id,
        material_id);

    auto program_desc =
        ProgramDesc()
            .add_path(
                SlangShaderCompiler::get_shader_dir(ShaderDirType::GPUAssembler)
                    .string() +
                "/instancer.slang")
            .set_entry_name("main")
            .set_shader_type(nvrhi::ShaderType::Compute);

    if (translations) {
        program_desc.define("HAS_TRANSLATIONS", "1");
    }

    if (rotations) {
        program_desc.define("HAS_ROTATIONS", "1");
    }

    if (scales) {
        program_desc.define("HAS_SCALES", "1");
    }

    if (instanceTransforms) {
        program_desc.define("HAS_INSTANCE_TRANSFORMS", "1");
    }

    ProgramHandle filler_program =
        get_instance().sa_resource_allocator.create(program_desc);

    // DIAGNOSTIC: print the slang compile error if the instancer shader failed.
    if (!filler_program) {
        spdlog::error("[fill_instances] instancer.slang program is NULL");
    }
    else if (!filler_program->get_error_string().empty()) {
        spdlog::error("[fill_instances] instancer.slang compile failed: {}",
                      filler_program->get_error_string());
    }

    MARK_DESTROY_NVRHI_RESOURCE(filler_program);

    ProgramVars filler_program_vars(
        get_instance().sa_resource_allocator, filler_program);

    auto device = RHI::get_device();

    // Create index buffer
    nvrhi::BufferDesc index_desc =
        nvrhi::BufferDesc{}
            .setByteSize(instance_indices.size() * sizeof(int))
            .setStructStride(sizeof(int))
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName("instance_indices");
    auto index_buffer = get_instance().sa_resource_allocator.create(index_desc);
    MARK_DESTROY_NVRHI_RESOURCE(index_buffer);

    // Create single command list for all operations
    auto cmd = get_instance().sa_resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(cmd);

    cmd->open();
    cmd->writeBuffer(
        index_buffer,
        instance_indices.data(),
        instance_indices.size() * sizeof(int));

    // Create optional buffers
    nvrhi::BufferHandle translations_buffer;
    if (translations) {
        nvrhi::BufferDesc translations_desc =
            nvrhi::BufferDesc{}
                .setByteSize(instance_indices.size() * sizeof(GfVec3f))
                .setStructStride(sizeof(GfVec3f))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("translations");
        translations_buffer =
            get_instance().sa_resource_allocator.create(translations_desc);
        cmd->writeBuffer(
            translations_buffer,
            translations,
            instance_indices.size() * sizeof(GfVec3f));
    }
    MARK_DESTROY_NVRHI_RESOURCE(translations_buffer);

    nvrhi::BufferHandle rotations_buffer;
    if (rotations) {
        nvrhi::BufferDesc rotations_desc =
            nvrhi::BufferDesc{}
                .setByteSize(instance_indices.size() * sizeof(GfQuatf))
                .setStructStride(sizeof(GfQuatf))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("rotations");
        rotations_buffer =
            get_instance().sa_resource_allocator.create(rotations_desc);
        cmd->writeBuffer(
            rotations_buffer,
            rotations,
            instance_indices.size() * sizeof(GfQuatf));
    }
    MARK_DESTROY_NVRHI_RESOURCE(rotations_buffer);

    nvrhi::BufferHandle scales_buffer;
    if (scales) {
        nvrhi::BufferDesc scales_desc =
            nvrhi::BufferDesc{}
                .setByteSize(instance_indices.size() * sizeof(GfVec3f))
                .setStructStride(sizeof(GfVec3f))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("scales");
        scales_buffer =
            get_instance().sa_resource_allocator.create(scales_desc);
        cmd->writeBuffer(
            scales_buffer, scales, instance_indices.size() * sizeof(GfVec3f));
    }
    MARK_DESTROY_NVRHI_RESOURCE(scales_buffer);

    nvrhi::BufferHandle instance_transforms_buffer;
    if (instanceTransforms) {
        nvrhi::BufferDesc instance_transforms_desc =
            nvrhi::BufferDesc{}
                .setByteSize(instance_indices.size() * sizeof(GfMatrix4d))
                .setStructStride(sizeof(GfMatrix4d))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("instance_transforms");
        instance_transforms_buffer =
            get_instance().sa_resource_allocator.create(
                instance_transforms_desc);
        cmd->writeBuffer(
            instance_transforms_buffer,
            instanceTransforms,
            instance_indices.size() * sizeof(GfMatrix4d));
    }
    MARK_DESTROY_NVRHI_RESOURCE(instance_transforms_buffer);

    cmd->close();
    device->executeCommandList(cmd);

    // Set up constant buffer parameters
    struct InstanceParams {
        GfMatrix4f parent_transform;
        GfMatrix4f prototype_transform;
        unsigned material_id;
        unsigned geometry_id;
        unsigned instance_count;
        unsigned base_instance_index;
        uint64_t BLAS_address;
    };

    InstanceParams params;
    params.parent_transform = parent_transform;
    params.prototype_transform = prototype_transform;
    params.material_id = material_id;
    params.geometry_id = geometry_id;
    params.instance_count = instance_indices.size();
    params.base_instance_index = geometry_instance_buffer->index();
    params.BLAS_address = BLAS_address;

    nvrhi::BufferDesc params_desc =
        nvrhi::BufferDesc{}
            .setByteSize(sizeof(InstanceParams))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("instance_params");
    auto params_buffer =
        get_instance().sa_resource_allocator.create(params_desc);
    MARK_DESTROY_NVRHI_RESOURCE(params_buffer);

    cmd->open();
    cmd->writeBuffer(params_buffer, &params, sizeof(InstanceParams));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();

    // Set up program variables
    filler_program_vars["g_Params"] = params_buffer;
    filler_program_vars["g_InstanceIndices"] = index_buffer;
    filler_program_vars["g_GeometryInstances"] =
        geometry_instance_buffer->get_descriptor();

    // Important: Use RawBuffer_UAV for RWByteAddressBuffer in shader
    filler_program_vars["g_RTInstances"] =
        rt_instance_buffer->get_descriptor(nvrhi::ResourceType::RawBuffer_UAV);

    if (translations) {
        filler_program_vars["g_Translations"] = translations_buffer;
    }
    if (rotations) {
        filler_program_vars["g_Rotations"] = rotations_buffer;
    }
    if (scales) {
        filler_program_vars["g_Scales"] = scales_buffer;
    }
    if (instanceTransforms) {
        filler_program_vars["g_InstanceTransforms"] =
            instance_transforms_buffer;
    }

    filler_program_vars.finish_setting_vars();

    ComputeContext compute_context(
        get_instance().sa_resource_allocator, filler_program_vars);
    compute_context.finish_setting_pso();

    compute_context.begin();
    compute_context.dispatch(
        {}, filler_program_vars, instance_indices.size(), 64);
    compute_context.finish();

    device->waitForIdle();
}

void GPUSceneAssember::compute_sphere_aabbs(
    nvrhi::BufferHandle vertex_buffer,
    size_t positions_offset,
    size_t radii_offset,
    uint32_t sphere_count,
    nvrhi::IBuffer* out_aabb_buffer)
{
    spdlog::info(
        "GPUSceneAssember::compute_sphere_aabbs: Computing AABBs for {} "
        "spheres (posOffset={}, radiiOffset={})",
        sphere_count,
        positions_offset,
        radii_offset);

    auto program_desc =
        ProgramDesc()
            .add_path(
                SlangShaderCompiler::get_shader_dir(ShaderDirType::GPUAssembler)
                    .string() +
                "/compute_sphere_aabbs.slang")
            .set_entry_name("main")
            .set_shader_type(nvrhi::ShaderType::Compute);

    ProgramHandle compute_program =
        get_instance().sa_resource_allocator.create(program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(compute_program);

    ProgramVars program_vars(
        get_instance().sa_resource_allocator, compute_program);

    auto device = RHI::get_device();

    // Create params constant buffer
    struct Params {
        uint32_t sphere_count;
        uint32_t positions_offset;
        uint32_t radii_offset;
        uint32_t padding;
    };

    Params params;
    params.sphere_count = sphere_count;
    params.positions_offset = static_cast<uint32_t>(positions_offset);
    params.radii_offset = static_cast<uint32_t>(radii_offset);

    nvrhi::BufferDesc params_desc =
        nvrhi::BufferDesc{}
            .setByteSize(sizeof(Params))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("sphere_aabb_params");

    auto params_buffer =
        get_instance().sa_resource_allocator.create(params_desc);
    MARK_DESTROY_NVRHI_RESOURCE(params_buffer);

    auto cmd = get_instance().sa_resource_allocator.create(CommandListDesc{});
    MARK_DESTROY_NVRHI_RESOURCE(cmd);

    cmd->open();
    cmd->writeBuffer(params_buffer, &params, sizeof(Params));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();

    // Set up program variables
    program_vars["Params"] = params_buffer;
    program_vars["g_VertexBuffer"] = vertex_buffer;
    program_vars["g_OutputAABBs"] = out_aabb_buffer;

    program_vars.finish_setting_vars();

    ComputeContext compute_context(
        get_instance().sa_resource_allocator, program_vars);
    compute_context.finish_setting_pso();

    compute_context.begin();
    compute_context.dispatch({}, program_vars, sphere_count, 64);
    compute_context.finish();

    spdlog::info(
        "GPUSceneAssember::compute_sphere_aabbs: AABB computation complete");
}

RUZINO_NAMESPACE_CLOSE_SCOPE