// VolumeImpl shared implementation: the create() factory + the
// buildSingleAabbBlas helper used by all volume types.
#include "volume_impl.h"

#include <mutex>
#include <string>

#include "cloud_volume_impl.h"
#include "internal/memory/DeviceMemoryPool.hpp"  // execution_launch_mutex
#include "nvrhi/nvrhi.h"
#include "nvrhi/utils.h"
#include "wetbrush_volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE

// Decide the concrete impl from the `volumeType` primvar.
//   "cloud"            -> CloudVolumeImpl
//   absent / "wetbrush" -> WetbrushVolumeImpl (default)
// If `previous` is already the right type, reuse it so cached primvars /
// registry versions survive across dirty cycles; otherwise replace it.
void VolumeImpl::resolve(
    HdSceneDelegate* sceneDelegate,
    const SdfPath& id,
    std::unique_ptr<VolumeImpl>& previous)
{
    VtValue vt_type = sceneDelegate->Get(id, TfToken("volumeType"));
    bool want_cloud = false;
    if (vt_type.IsHolding<TfToken>()) {
        want_cloud = vt_type.UncheckedGet<TfToken>().GetString() == "cloud";
    }
    else if (vt_type.IsHolding<std::string>()) {
        want_cloud = vt_type.UncheckedGet<std::string>() == "cloud";
    }

    // Reuse the existing impl if its type still matches — keeps cached state.
    if (previous) {
        if (want_cloud && dynamic_cast<CloudVolumeImpl*>(previous.get()))
            return;
        if (!want_cloud && dynamic_cast<WetbrushVolumeImpl*>(previous.get()))
            return;
        // Type switched: drop the old impl, fall through to construct fresh.
        previous.reset();
    }

    if (want_cloud)
        previous = std::make_unique<CloudVolumeImpl>();
    else
        previous = std::make_unique<WetbrushVolumeImpl>();
}

void buildSingleAabbBlas(
    nvrhi::IDevice* device,
    GfVec3f boundsMin,
    GfVec3f boundsMax,
    nvrhi::BufferHandle& aabbBuffer,
    nvrhi::rt::AccelStructHandle& BLAS,
    nvrhi::CommandListHandle& command_list,
    nvrhi::CommandListHandle& copy_commandlist,
    const std::string& debugName)
{
    const size_t aabb_bytes = sizeof(nvrhi::rt::GeometryAABB);
    if (!aabbBuffer || aabbBuffer->getDesc().byteSize != aabb_bytes) {
        nvrhi::BufferDesc aabb_desc =
            nvrhi::BufferDesc{}
                .setByteSize(aabb_bytes)
                .setStructStride(sizeof(nvrhi::rt::GeometryAABB))
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setCpuAccess(nvrhi::CpuAccessMode::None)
                .setIsAccelStructBuildInput(true)
                .setKeepInitialState(true)
                .setDebugName(debugName);
        aabbBuffer = device->createBuffer(aabb_desc);
    }

    nvrhi::rt::GeometryAABB aabb;
    aabb.minX = boundsMin[0];
    aabb.minY = boundsMin[1];
    aabb.minZ = boundsMin[2];
    aabb.maxX = boundsMax[0];
    aabb.maxY = boundsMax[1];
    aabb.maxZ = boundsMax[2];

    // Build the BLAS once (first time). On later calls only the AABB buffer
    // contents are refreshed — the accel struct itself is not rebuilt (matches
    // the existing wetbrush/cloud behaviour; the TLAS only needs the bounds at
    // build time). Hold execution_launch_mutex across the whole upload + build,
    // matching the mesh.cpp/points.cpp/light.cpp convention (the old wetbrush
    // BLAS build was submitted WITHOUT the lock — an inconsistency this fixes).
    if (!BLAS) {
        std::lock_guard lock(execution_launch_mutex);
        copy_commandlist->open();
        copy_commandlist->writeBuffer(aabbBuffer, &aabb, aabb_bytes);
        copy_commandlist->close();
        device->executeCommandList(copy_commandlist);
        device->waitForIdle();

        nvrhi::rt::AccelStructDesc blas_desc;
        nvrhi::rt::GeometryDesc geometry_desc;
        geometry_desc.geometryType = nvrhi::rt::GeometryType::AABBs;
        geometry_desc.useTransform = false;
        nvrhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.setBuffer(aabbBuffer)
            .setCount(1)
            .setStride(sizeof(nvrhi::rt::GeometryAABB))
            .setOffset(0);
        geometry_desc.setAABBs(aabbGeometry);
        blas_desc.addBottomLevelGeometry(geometry_desc);
        blas_desc.isTopLevel = false;
        BLAS = device->createAccelStruct(blas_desc);
        if (!command_list)
            command_list = device->createCommandList();
        command_list->open();
        nvrhi::utils::BuildBottomLevelAccelStruct(
            command_list, BLAS, blas_desc);
        command_list->close();
        device->executeCommandList(command_list);
        device->waitForIdle();
    }
    else {
        // BLAS already built — just refresh the AABB buffer contents.
        std::lock_guard lock(execution_launch_mutex);
        copy_commandlist->open();
        copy_commandlist->writeBuffer(aabbBuffer, &aabb, aabb_bytes);
        copy_commandlist->close();
        device->executeCommandList(copy_commandlist);
        device->waitForIdle();
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
