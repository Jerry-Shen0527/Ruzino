//
// Hd_RUZINO_WetbrushVolume implementation.
//
// Carries a 2D canvas layer (gridRes x gridRes Float4 cells of density,r,g,b)
// plus the slab world-AABB, and registers a procedural-AABB BLAS routed to
// hit-group 4/5 so the VolumeIntersection/VolumeClosestHit/VolumeShadowHit
// shaders raymarch the density slab. Mirrors Hd_RUZINO_Points' BLAS/TLAS
// pattern but with a single CPU-computed AABB (no GPU compute pass) and a
// dedicated VolumeDesc pool entry carrying the bindless density index.
//
#include "wetbrush_volume.h"

#include <spdlog/spdlog.h>

#include "../instancer.h"
#include "../renderParam.h"
#include "RHI/shared_buffer_registry.hpp"
#include "Scene/SceneTypes.slang"
#include "material/material.h"
#include "nvrhi/utils.h"
#include "pxr/imaging/hd/instancer.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_WetbrushVolume::Hd_RUZINO_WetbrushVolume(const SdfPath& id)
    : HdVolume(id)
{
    auto device = RHI::get_device();
    copy_commandlist = device->createCommandList();
}

Hd_RUZINO_WetbrushVolume::~Hd_RUZINO_WetbrushVolume()
{
}

HdDirtyBits Hd_RUZINO_WetbrushVolume::GetInitialDirtyBitsMask() const
{
    int mask = HdChangeTracker::Clean | HdChangeTracker::InitRepr |
               HdChangeTracker::DirtyTransform |
               HdChangeTracker::DirtyVisibility |
               HdChangeTracker::DirtyPrimvar |
               HdChangeTracker::DirtyInstancer |
               HdChangeTracker::DirtyMaterialId;
    return (HdDirtyBits)mask;
}

HdDirtyBits Hd_RUZINO_WetbrushVolume::_PropagateDirtyBits(HdDirtyBits bits)
    const
{
    return bits;
}

void Hd_RUZINO_WetbrushVolume::create_gpu_resources(
    Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();
    if (!copy_commandlist)
        copy_commandlist = device->createCommandList();

    size_t cell_count = static_cast<size_t>(gridResX) * gridResY * gridResZ;

    // ------------------------------------------------------------------
    // Phase 1: check the shared GPU buffer registry for a zero-copy buffer
    // produced by the simulation. If hit, use it directly (skip createBuffer +
    // writeBuffer entirely). The metadata blob carries the grid geometry.
    //
    // DISABLED: the zero-copy registry path currently produces near-black
    // output (frame 0 ~0.3% lit, frame 1+ 0%). The sim's packed_paint buffer
    // is created with UAV/TypedView flags (brush_create_typed_buffer) but the
    // renderer binds it as a RawBuffer (ByteAddressBuffer) SRV -- the view
    // mismatch plus a missing UA→ShaderResource state transition after the
    // sim's pack_float4 dispatch leave the shader reading zeroes. Falling back
    // to the primvar path (Phase 2) renders correctly. Re-enable once the
    // buffer flags + post-dispatch barrier are fixed. See SESSION_CONTEXT.md.
    // ------------------------------------------------------------------
    nvrhi::BufferHandle ext_buf;
    size_t ext_bytes = 0;
    uint64_t ext_version = 0;
    const void* ext_meta_ptr = nullptr;
    size_t ext_meta_bytes = 0;
    bool registry_hit = false;  // SharedGPUBufferRegistry path disabled (see above)
    (void)ext_buf; (void)ext_bytes; (void)ext_version;
    (void)ext_meta_ptr; (void)ext_meta_bytes;

    if (registry_hit && ext_buf && ext_meta_bytes >= 7 * sizeof(float)) {
        // Pull grid geometry from the registry metadata (agreed POD layout:
        // uint32 resX, resY, resZ; float cellSize, gridMinX, gridMinY, gridMinZ).
        struct PaintFieldMeta {
            uint32_t resX, resY, resZ;
            float cellSize;
            float gridMinX, gridMinY, gridMinZ;
        };
        auto* meta = static_cast<const PaintFieldMeta*>(ext_meta_ptr);
        gridResX = meta->resX;
        gridResY = meta->resY;
        gridResZ = meta->resZ;
        cellSize = meta->cellSize;
        gridMin = GfVec3f(meta->gridMinX, meta->gridMinY, meta->gridMinZ);
        cell_count = static_cast<size_t>(gridResX) * gridResY * gridResZ;

        // Use the external buffer. Only (re)register the bindless descriptor
        // when the buffer handle or version changes (first time, or sim
        // reallocated the buffer).
        if (!densityDescriptorHandle.Get() ||
            densityBuffer.Get() != ext_buf.Get() ||
            registryVersion != ext_version) {
            densityBuffer = ext_buf;
            registryVersion = ext_version;
            auto descriptor_table =
                render_param->InstanceCollection->get_buffer_descriptor_table();
            densityDescriptorHandle = descriptor_table->CreateDescriptorHandle(
                nvrhi::BindingSetItem::RawBuffer_SRV(0, densityBuffer));
            spdlog::info(
                "WetbrushVolume {}: registry hit (v{}, {}x{}x{}, {} bytes, "
                "bindless={})",
                GetId().GetText(), ext_version,
                gridResX, gridResY, gridResZ, ext_bytes,
                densityDescriptorHandle.Get());
        }
    } else if (cell_count == 0 || paintField.size() != cell_count) {
        // Phase 2 fallback requires primvar data — if it's missing/empty, skip.
        spdlog::warn(
            "WetbrushVolume {}: no registry buffer and paint field mismatch "
            "({}x{}x{} voxels, {} entries) -- skipping GPU resources",
            GetId().GetText(), gridResX, gridResY, gridResZ,
            paintField.size());
        _valid = false;
        return;
    } else {
        // ------------------------------------------------------------------
        // Phase 2 fallback: create our own buffer from the primvar data
        // (Float4 per voxel: density, r, g, b).
        // ------------------------------------------------------------------
        const size_t density_bytes = cell_count * sizeof(GfVec4f);
        if (!densityBuffer ||
            densityBuffer->getDesc().byteSize != density_bytes) {
            nvrhi::BufferDesc desc =
                nvrhi::BufferDesc{}
                    .setCanHaveRawViews(true)
                    .setByteSize(density_bytes)
                    .setStructStride(sizeof(GfVec4f))
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setCpuAccess(nvrhi::CpuAccessMode::None)
                    .setKeepInitialState(true)
                    .setDebugName("wetbrushDensityBuffer");
            densityBuffer = device->createBuffer(desc);

            auto descriptor_table =
                render_param->InstanceCollection->get_buffer_descriptor_table();
            densityDescriptorHandle = descriptor_table->CreateDescriptorHandle(
                nvrhi::BindingSetItem::RawBuffer_SRV(0, densityBuffer));
        }

        // Upload the per-voxel (density, r, g, b) data.
        copy_commandlist->open();
        copy_commandlist->writeBuffer(
            densityBuffer, paintField.data(), density_bytes);
        copy_commandlist->close();
        {
            std::lock_guard lock(execution_launch_mutex);
            device->executeCommandList(copy_commandlist);
        }
    }

    // ------------------------------------------------------------------
    // Grid AABB: the 3D density grid's world bounds (gridMin + res*cellSize).
    // One procedural AABB covers the whole grid; the shader raymarches inside.
    // ------------------------------------------------------------------
    const GfVec3f bounds_min = gridMin;
    const GfVec3f bounds_max = GfVec3f(
        gridMin[0] + cellSize * static_cast<float>(gridResX),
        gridMin[1] + cellSize * static_cast<float>(gridResY),
        gridMin[2] + cellSize * static_cast<float>(gridResZ));

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
                .setDebugName("wetbrushVolumeAABB");
        aabbBuffer = device->createBuffer(aabb_desc);
    }

    nvrhi::rt::GeometryAABB aabb;
    aabb.minX = bounds_min[0];
    aabb.minY = bounds_min[1];
    aabb.minZ = bounds_min[2];
    aabb.maxX = bounds_max[0];
    aabb.maxY = bounds_max[1];
    aabb.maxZ = bounds_max[2];

    {
        std::lock_guard lock(execution_launch_mutex);
        copy_commandlist->open();
        copy_commandlist->writeBuffer(aabbBuffer, &aabb, aabb_bytes);
        copy_commandlist->close();
        device->executeCommandList(copy_commandlist);
        device->waitForIdle();
    }

    // ------------------------------------------------------------------
    // BLAS from the single procedural AABB (rebuilt only when first created or
    // the AABB buffer was recreated).
    // ------------------------------------------------------------------
    bool need_blas_rebuild = !BLAS;
    if (need_blas_rebuild) {
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

    // ------------------------------------------------------------------
    // VolumeDesc pool entry (3D grid metadata for the shader).
    // ------------------------------------------------------------------
    VolumeDesc vd;
    vd.bindlessIndex = densityDescriptorHandle.Get();
    vd.gridResX = gridResX;
    vd.gridResY = gridResY;
    vd.gridResZ = gridResZ;
    vd.cellSize = cellSize;
    vd.gridMin = float3(gridMin[0], gridMin[1], gridMin[2]);
    vd.boundsMin = float3(bounds_min[0], bounds_min[1], bounds_min[2]);
    vd.boundsMax = float3(bounds_max[0], bounds_max[1], bounds_max[2]);
    vd.padding = 0;

    if (!volume_desc_buffer)
        volume_desc_buffer =
            render_param->InstanceCollection->volume_pool.allocate(1);
    volume_desc_buffer->write_data(&vd);

    _valid = true;
    spdlog::info(
        "WetbrushVolume {}: created density buffer ({} cells, bindless={}), "
        "slab AABB [({:.3f},{:.3f},{:.3f})..({:.3f},{:.3f},{:.3f})]",
        GetId().GetText(),
        cell_count,
        vd.bindlessIndex,
        bounds_min[0], bounds_min[1], bounds_min[2],
        bounds_max[0], bounds_max[1], bounds_max[2]);
}

void Hd_RUZINO_WetbrushVolume::updateTLAS(
    Hd_RUZINO_RenderParam* render_param,
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits* dirtyBits)
{
    _UpdateInstancer(sceneDelegate, dirtyBits);
    const SdfPath& id = GetId();
    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());

    auto material_id = GetMaterialId();
    Hd_RUZINO_Material* material = nullptr;
    auto it = render_param->material_map->find(material_id);
    if (it != render_param->material_map->end() && it->second)
        material = it->second;

    // One slab instance (no instancer path for the canvas layer).
    auto& rt_instance_pool = render_param->InstanceCollection->rt_instance_pool;
    if (!rt_instanceBuffer || rt_instanceBuffer->count() != 1)
        rt_instanceBuffer = rt_instance_pool.allocate(1);
    if (!instanceBuffer || instanceBuffer->count() != 1)
        instanceBuffer =
            render_param->InstanceCollection->instance_pool.allocate(1);

    if (material)
        material->ensure_material_data_handle(render_param);

    GeometryInstanceData instance_data;
    instance_data.geometryID = volume_desc_buffer->index();
    instance_data.materialID = material ? material->GetMaterialLocation() : -1;
    memcpy(&instance_data.transform, transform.data(), sizeof(pxr::GfMatrix4f));
    // Stamp the type bits so the shader can recognise the volume instance
    // (flags top 3 bits encode GeometryType::Custom).
    instance_data.flags =
        static_cast<uint32_t>(GeometryType::Custom)
        << GeometryInstanceData::kTypeOffset;
    instanceBuffer->write_data(&instance_data);

    nvrhi::rt::InstanceDesc rt_instance;
    rt_instance.blasDeviceAddress = BLAS->getDeviceAddress();
    rt_instance.instanceMask = 1;
    rt_instance.instanceContributionToHitGroupIndex =
        4;  // Use volume hit groups (VolumeClosestHit / VolumeShadowHit)
    rt_instance.flags = nvrhi::rt::InstanceFlags::None;

    GfMatrix4f mat_transposed = transform.GetTranspose();
    memcpy(
        rt_instance.transform,
        mat_transposed.data(),
        sizeof(nvrhi::rt::AffineTransform));
    rt_instance.instanceID = instanceBuffer->index();

    rt_instanceBuffer->write_data(&rt_instance);

    render_param->InstanceCollection->set_require_rebuild_tlas();
}

void Hd_RUZINO_WetbrushVolume::_InitRepr(
    const TfToken& reprToken,
    HdDirtyBits* dirtyBits)
{
}

void Hd_RUZINO_WetbrushVolume::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits,
    const TfToken& reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    const SdfPath& id = GetId();
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    bool update_gpu_resources = false;

    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    // Pull the canvas metadata + paint-field primvars.
    if (*dirtyBits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::InitRepr))
    {
        // 3D grid metadata primvars (authored by render_wetbrush.py bake).
        VtValue rx_v = sceneDelegate->Get(id, TfToken("gridResX"));
        VtValue ry_v = sceneDelegate->Get(id, TfToken("gridResY"));
        VtValue rz_v = sceneDelegate->Get(id, TfToken("gridResZ"));
        VtValue cs_v = sceneDelegate->Get(id, TfToken("cellSize"));
        VtValue gm_v = sceneDelegate->Get(id, TfToken("gridMin"));
        VtValue paint_v = sceneDelegate->Get(id, TfToken("paintField"));

        auto read_uint = [](const VtValue& v) -> uint32_t {
            if (v.IsHolding<int>())  return static_cast<uint32_t>(v.UncheckedGet<int>());
            if (v.IsHolding<uint>()) return static_cast<uint32_t>(v.UncheckedGet<uint>());
            return 0;
        };
        uint32_t new_rx = read_uint(rx_v);
        uint32_t new_ry = read_uint(ry_v);
        uint32_t new_rz = read_uint(rz_v);
        float new_cs = cs_v.IsHolding<float>() ? cs_v.UncheckedGet<float>() : 0.0f;
        GfVec3f new_gm = gm_v.IsHolding<GfVec3f>() ? gm_v.UncheckedGet<GfVec3f>()
                                                    : GfVec3f(0.0f);
        if (new_rx != gridResX || new_ry != gridResY || new_rz != gridResZ ||
            new_cs != cellSize || new_gm != gridMin) {
            gridResX = new_rx;
            gridResY = new_ry;
            gridResZ = new_rz;
            cellSize = new_cs;
            gridMin = new_gm;
            update_gpu_resources = true;
        }

        // Check if the shared-registry buffer has a new version (sim updated
        // the paint field this frame). The bindless SRV auto-tracks content
        // changes, so we only need to re-run create_gpu_resources when the
        // version bumps (buffer handle may have changed).
        nvrhi::BufferHandle _dummy;
        size_t _dummy_bytes;
        uint64_t reg_ver;
        if (SharedGPUBufferRegistry::get().lookup(
                "wetbrush_paint_field", _dummy, _dummy_bytes, reg_ver) &&
            reg_ver != registryVersion) {
            update_gpu_resources = true;
        }

        if (!paint_v.IsEmpty() && paint_v.IsHolding<VtVec4fArray>()) {
            const auto& pf = paint_v.UncheckedGet<VtVec4fArray>();
            paintField.assign(pf.begin(), pf.end());
            update_gpu_resources = true;
            spdlog::info(
                "WetbrushVolume {}: loaded 3D paint field ({} voxels, "
                "{}x{}x{})",
                id.GetText(), paintField.size(), gridResX, gridResY, gridResZ);
        }
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        SdfPath const& newMaterialId = sceneDelegate->GetMaterialId(id);
        if (GetMaterialId() != newMaterialId) {
            SetMaterialId(newMaterialId);
        }
    }

    if (update_gpu_resources && gridResX > 0 && !paintField.empty()) {
        create_gpu_resources(render_param);
    }

    if (_valid && BLAS) {
        updateTLAS(render_param, sceneDelegate, dirtyBits);
    }
    render_param->InstanceCollection->mark_geometry_dirty();

    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_WetbrushVolume::Finalize(HdRenderParam* renderParam)
{
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    if (instanceBuffer)
        instanceBuffer.reset();
    if (rt_instanceBuffer)
        rt_instanceBuffer.reset();
    if (volume_desc_buffer)
        volume_desc_buffer.reset();

    spdlog::info("Finalized WetbrushVolume {}", GetId().GetText());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
