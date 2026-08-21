#pragma once

#include "RHI/shaderCompiler.h"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "RHI/ShaderFactory/shader.hpp"
#include "api.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

// Forward declarations to avoid including slang file
#include "../nodes/shaders/Scene/SceneTypes.slang"
#include "internal/memory/DeviceMemoryPool.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

struct HD_RUZINO_API GPUSceneAssember {
    static void fill_instances(
        const pxr::GfMatrix4f& parent_transform,
        const pxr::VtIntArray& instance_indices,
        const pxr::GfVec3f* translations,
        const pxr::GfQuatf* rotations,
        const pxr::GfVec3f* scales,
        const pxr::GfMatrix4d* instanceTransforms,
        DeviceMemoryPool<GeometryInstanceData>::MemoryHandle
            geometry_instance_buffer,
        DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle
            rt_instance_buffer,
        uint64_t BLAS_address,
        const pxr::GfMatrix4f& prototype_transform,
        unsigned material_id,
        unsigned geometry_id);

    // Compute AABBs for spheres on GPU
    static void compute_sphere_aabbs(
        nvrhi::BufferHandle vertex_buffer,
        size_t positions_offset,
        size_t radii_offset,
        uint32_t sphere_count,
        nvrhi::IBuffer* out_aabb_buffer);

    // Compute AABBs for capsule segments on GPU (debug bristles). Each prim is
    // the capsule around segment [A, B]: AABB = min/max(A, B) ± radius.
    static void compute_segment_aabbs(
        nvrhi::BufferHandle vertex_buffer,
        size_t endpoints_a_offset,
        size_t endpoints_b_offset,
        size_t radii_offset,
        uint32_t segment_count,
        nvrhi::IBuffer* out_aabb_buffer);

    static GPUSceneAssember& get_instance()
    {
        return instance;
    }

    static void initialize_instance()
    {
        get_instance().shader_factory = std::make_unique<ShaderFactory>();
        get_instance().sa_resource_allocator.shader_factory =
            get_instance().shader_factory.get();
        get_instance().sa_resource_allocator.device = RHI::get_device();
        get_instance().sa_resource_allocator.shader_factory->add_search_path(
            SlangShaderCompiler::get_shader_dir(ShaderDirType::GPUAssembler)
                .string());
    }

    static void destroy_instance()
    {
        get_instance().sa_resource_allocator.terminate();
    }

    ResourceAllocator sa_resource_allocator;
    std::unique_ptr<ShaderFactory> shader_factory;

    // Cached AABB-compute resources. The debug points pipeline calls the
    // compute_*_aabbs helpers EVERY frame (registry version bump); creating
    // and destroying the program per call both stalls (slang recompile +
    // PSO recreation) and trips a use-after-free in the allocator's program
    // cache after a few cycles. Compile once, reuse the constant buffer.
    ProgramHandle sphere_aabb_program;
    ProgramHandle segment_aabb_program;
    nvrhi::BufferHandle sphere_aabb_params;
    nvrhi::BufferHandle segment_aabb_params;

    static GPUSceneAssember instance;
};

RUZINO_NAMESPACE_CLOSE_SCOPE