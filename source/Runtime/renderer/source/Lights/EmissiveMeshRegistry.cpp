#include "EmissiveMeshRegistry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "GPUContext/compute_context.hpp"
#include "GPUContext/program_vars.hpp"
#include "GPUContext/raytracing_context.hpp"
#include "LightBVHBuilder.h"
#include "RHI/rhi.hpp"
#include "RHI/shaderCompiler.h"
#include "gpu_compute.h"
#include "material/material.h"
#include "renderTLAS.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace {
auto& res_alloc()
{
    return GPUSceneAssember::get_instance().sa_resource_allocator;
}

float3 minF3(const float3& a, const float3& b)
{
    return float3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
float3 maxF3(const float3& a, const float3& b)
{
    return float3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
}

/// Work item matching the GPU shader's EmissiveMeshWorkItem struct.
/// Layout: 4 uint (16B) + float3 (12B) + 4 uint (16B) + pad = 48B.
/// float3 in slang is 12B with 4B alignment, same as float[3] in C++.
struct EmissiveMeshWorkItem {
    uint32_t instanceID;
    uint32_t meshDescIndex;
    uint32_t triangleOffset;
    uint32_t triangleCount;
    float emissionRadiance[3];
    uint32_t emissionTextureIndex;
    uint32_t materialLocation;
    uint32_t _pad0;
    uint32_t _pad1;
};
}  // namespace

bool EmissiveMeshRegistry::build_gpu_buffers(
    Hd_RUZINO_RenderInstanceCollection* collection,
    pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>* materials)
{
    auto device = RHI::get_device();
    if (!device || !collection)
        return false;

    uint32_t instancePoolSize =
        static_cast<uint32_t>(collection->instance_pool.pool_size());

    // Build location -> material* map for per-mesh isEmissive() filtering.
    std::unordered_map<uint32_t, Hd_RUZINO_Material*> locToMaterial;
    if (materials) {
        for (const auto& [path, mat] : *materials) {
            if (mat) {
                unsigned int loc = mat->GetMaterialLocation();
                if (loc != (unsigned int)-1) {
                    locToMaterial[loc] = mat;
                }
            }
        }
    }

    // Build the work list: one entry per emissive mesh. The CPU does ONLY the
    // lightweight filtering (isEmissive?) and metadata lookup. The GPU compute
    // pass does the heavy lifting (vertex transform, normal/area, encoding).
    std::vector<EmissiveMeshWorkItem> workList;
    std::vector<uint32_t> perInstanceOffset(
        instancePoolSize, MeshLightData::kInvalidIndex);

    uint32_t triOffset = 0;
    uint32_t meshIdx = 0;

    for (const auto& [instanceSlot, entry] : entries_) {
        if (instanceSlot >= instancePoolSize)
            continue;
        if (entry.triangleCount == 0)
            continue;

        uint32_t loc = entry.defaultMaterialLocation;
        if (loc == (unsigned int)-1)
            continue;

        auto matIt = locToMaterial.find(loc);
        if (matIt == locToMaterial.end())
            continue;
        Hd_RUZINO_Material* mat = matIt->second;
        if (!mat || !mat->isEmissive())
            continue;

        GfVec3f radiance = mat->getEmissionRadiance();
        if (radiance[0] <= 0 && radiance[1] <= 0 && radiance[2] <= 0)
            continue;

        EmissiveMeshWorkItem item;
        item.instanceID = instanceSlot;
        item.meshDescIndex = entry.meshDescIndex;
        item.triangleOffset = triOffset;
        item.triangleCount = entry.triangleCount;
        item.emissionRadiance[0] = radiance[0];
        item.emissionRadiance[1] = radiance[1];
        item.emissionRadiance[2] = radiance[2];
        item.emissionTextureIndex = mat->getEmissionTextureIndex();
        item.materialLocation = loc;
        item._pad0 = 0;
        item._pad1 = 0;
        workList.push_back(item);

        perInstanceOffset[instanceSlot] = triOffset;

        triOffset += entry.triangleCount;
        meshIdx++;
    }

    triangleCount_ = triOffset;
    meshCount_ = meshIdx;

    if (triOffset == 0) {
        emissiveTrianglePool.clear();
        emissiveFluxPool.clear();
        emissiveMeshPool.clear();
        emissivePerInstanceOffsetPool.clear();
        return false;
    }

    // Allocate output pools (clear + re-allocate each build).
    emissiveTrianglePool.clear();
    emissiveFluxPool.clear();
    emissiveMeshPool.clear();
    emissivePerInstanceOffsetPool.clear();

    triHandle = emissiveTrianglePool.allocate(triOffset);
    fluxHandle = emissiveFluxPool.allocate(triOffset);
    meshHandle = emissiveMeshPool.allocate(meshIdx);
    offsetHandle = emissivePerInstanceOffsetPool.allocate(instancePoolSize);

    // Write per-instance offset table CPU-side (pre-fill for correctness).
    offsetHandle->write_data(perInstanceOffset.data());

    // Build MeshLightData entries.
    std::vector<MeshLightData> meshDataArr;
    meshDataArr.reserve(meshIdx);
    for (size_t i = 0; i < workList.size(); i++) {
        const auto& w = workList[i];
        MeshLightData mld;
        mld.instanceID = w.instanceID;
        mld.triangleOffset = w.triangleOffset;
        mld.triangleCount = w.triangleCount;
        mld.materialID = w.materialLocation;
        meshDataArr.push_back(mld);
    }
    meshHandle->write_data(meshDataArr.data());

    // --- GPU compute pass ---
    auto program_desc =
        ProgramDesc()
            .add_path(
                SlangShaderCompiler::get_shader_dir(ShaderDirType::GPUAssembler)
                    .string() +
                "/build_emissive_triangles.cs.slang")
            .set_entry_name("main")
            .set_shader_type(nvrhi::ShaderType::Compute);

    ProgramHandle program = res_alloc().create(program_desc);
    if (!program || !program->get_error_string().empty()) {
        spdlog::error(
            "[EmissiveMeshRegistry] build_emissive_triangles.cs.slang "
            "compile failed: {}",
            program ? program->get_error_string() : "program is null");
        return false;
    }
    // Return the program to the allocator's pool on exit (the same rent/
    // return idiom the render nodes use). The pool re-hands out the compiled
    // program on the next build, so repeated geometry edits don't re-run the
    // slang compiler. Not returning it here meant every build was a pool miss
    // and paid a full recompile.
    struct ProgramReturner {
        ResourceAllocator& allocator;
        ProgramHandle& handle;
        ~ProgramReturner()
        {
            allocator.destroy(handle);
        }
    } program_returner{ res_alloc(), program };

    ProgramVars vars(res_alloc(), program);

    // Upload work list to a temporary buffer.
    nvrhi::BufferDesc workListDesc =
        nvrhi::BufferDesc{}
            .setByteSize(workList.size() * sizeof(EmissiveMeshWorkItem))
            .setStructStride(sizeof(EmissiveMeshWorkItem))
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName("emissiveMeshWorkList");
    auto workListBuffer = res_alloc().create(workListDesc);

    auto cmd = res_alloc().create(CommandListDesc{});
    cmd->open();
    cmd->writeBuffer(
        workListBuffer,
        workList.data(),
        workList.size() * sizeof(EmissiveMeshWorkItem));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();

    // Constants.
    struct Params {
        uint32_t totalTriangles;
        uint32_t meshCount;
        uint32_t instancePoolSize;
        uint32_t pad;
    };
    Params params{ triOffset, meshIdx, instancePoolSize, 0 };

    nvrhi::BufferDesc paramsDesc =
        nvrhi::BufferDesc{}
            .setByteSize(sizeof(Params))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("emissiveBuildParams");
    auto paramsBuffer = res_alloc().create(paramsDesc);
    cmd->open();
    cmd->writeBuffer(paramsBuffer, &params, sizeof(Params));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    // Command list is only used for the two uploads above — return it to the
    // allocator pool. Without this, every geometry edit leaked one
    // CommandList (261 edits in a session = 261 leaked command lists).
    res_alloc().destroy(cmd);

    // Bind resources.
    vars["g_Params"] = paramsBuffer;
    vars["g_MeshWorkList"] = workListBuffer;

    // Output UAVs.
    vars["g_EmissiveTriangles"] =
        triHandle->get_descriptor(nvrhi::ResourceType::StructuredBuffer_UAV);
    vars["g_EmissiveFlux"] =
        fluxHandle->get_descriptor(nvrhi::ResourceType::StructuredBuffer_UAV);
    vars["g_PerInstanceOffset"] =
        offsetHandle->get_descriptor(nvrhi::ResourceType::StructuredBuffer_UAV);

    // Read-only SRVs: instance + mesh descriptors (from the collection's
    // pools).
    vars["instanceDescBuffer"] = collection->instance_pool.get_device_buffer();
    vars["meshDescBuffer"] = collection->mesh_pool.get_device_buffer();

    // Bindless descriptor table (t_BindlessBuffers) — same pattern as
    // path_tracing.cpp.
    vars.set_descriptor_table(
        "t_BindlessBuffers",
        collection->bindlessData.bufferDescriptorTableManager
            ->GetDescriptorTable(),
        collection->bindlessData.bufferBindlessLayout);

    vars.finish_setting_vars();

    ComputeContext compute_ctx(res_alloc(), vars);
    compute_ctx.finish_setting_pso();
    compute_ctx.begin();
    compute_ctx.dispatch({}, vars, triOffset, 64);
    compute_ctx.finish();
    device->waitForIdle();
    // The GPU is done reading the upload buffers (waitForIdle above) — return
    // them to the allocator pool. Without this, every geometry edit leaked
    // two Buffers (work list + params), growing unboundedly across a session.
    res_alloc().destroy(workListBuffer);
    res_alloc().destroy(paramsBuffer);

    // --- Build LightBVH (CPU) ---
    // Read back the emissive triangle data from the GPU, compute per-triangle
    // sort data (AABB, centroid, normal, flux), build the BVH, and upload the
    // node/triangleIndex/triangleBitmask buffers.
    auto bvh_t0 = std::chrono::high_resolution_clock::now();
    {
        std::vector<PackedEmissiveTriangle> readbackTris(triOffset);
        triHandle->read_data(readbackTris.data());

        std::vector<EmissiveFlux> readbackFlux(triOffset);
        fluxHandle->read_data(readbackFlux.data());

        auto readback_t1 = std::chrono::high_resolution_clock::now();

        std::vector<TriangleSortData> sortData(triOffset);
        for (uint32_t i = 0; i < triOffset; i++) {
            EmissiveTriangle et = readbackTris[i].unpack();

            float3 bmin = minF3(minF3(et.posW[0], et.posW[1]), et.posW[2]);
            float3 bmax = maxF3(maxF3(et.posW[0], et.posW[1]), et.posW[2]);
            sortData[i].boundsMin = bmin;
            sortData[i].boundsMax = bmax;
            sortData[i].center = (bmin + bmax) * 0.5f;
            sortData[i].coneDirection = et.normal;
            sortData[i].cosConeAngle = 1.0f;
            sortData[i].flux = readbackFlux[i].flux;
            sortData[i].triangleIndex = i;
        }

        auto sort_t1 = std::chrono::high_resolution_clock::now();

        std::vector<PackedNode> bvhNodes;
        std::vector<uint32_t> bvhTriIndices;
        std::vector<uint2> bvhBitmasks;

        LightBVHBuildOptions bvhOpts;
        bvhOpts.maxTriangleCountPerLeaf = 4;  // split early for deeper BVH
        LightBVHBuilder builder;
        if (builder.build(
                sortData, bvhOpts, bvhNodes, bvhTriIndices, bvhBitmasks)) {
            auto build_t1 = std::chrono::high_resolution_clock::now();

            bvhNodePool.clear();
            bvhTriangleIndexPool.clear();
            bvhTriangleBitmaskPool.clear();

            bvhNodeHandle =
                bvhNodePool.allocate(static_cast<uint32_t>(bvhNodes.size()));
            bvhTriIdxHandle = bvhTriangleIndexPool.allocate(
                static_cast<uint32_t>(bvhTriIndices.size()));
            bvhBitmaskHandle = bvhTriangleBitmaskPool.allocate(
                static_cast<uint32_t>(bvhBitmasks.size()));

            bvhNodeHandle->write_data(bvhNodes.data());
            bvhTriIdxHandle->write_data(bvhTriIndices.data());
            bvhBitmaskHandle->write_data(bvhBitmasks.data());

            auto upload_t1 = std::chrono::high_resolution_clock::now();

            auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            spdlog::info(
                "[EmissiveMeshRegistry] BVH build timing for {} triangles: "
                "readback={:.2f}ms sortdata={:.2f}ms bvh_build={:.2f}ms "
                "upload={:.2f}ms total={:.2f}ms ({} nodes)",
                triOffset,
                ms(bvh_t0, readback_t1),
                ms(readback_t1, sort_t1),
                ms(sort_t1, build_t1),
                ms(build_t1, upload_t1),
                ms(bvh_t0, upload_t1),
                bvhNodes.size());
        }
    }

    return true;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
