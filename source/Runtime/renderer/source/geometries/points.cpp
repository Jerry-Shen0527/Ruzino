//
// Point/Sphere Geometry Implementation
//
#include "points.h"

#include <spdlog/spdlog.h>

#include "../gpu_compute.h"
#include "../hydra2Ingest.h"
#include "../instancer.h"
#include "../renderParam.h"
#include "RHI/shared_buffer_registry.hpp"
#include "Scene/SceneTypes.slang"
#include "material/material.h"
#include "nvrhi/utils.h"
#include "pxr/imaging/hd/instancer.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

// Active registry-mode points prims, refreshed per frame from the render
// thread callback (see refresh_debug_prims). Guarded by its own mutex.
static std::mutex s_debug_prims_mutex;
static std::vector<Hd_RUZINO_Points*> s_debug_prims;

Hd_RUZINO_Points::Hd_RUZINO_Points(const SdfPath& id)
    : HdPoints(id),
      _pointsValid(false)
{
    auto device = RHI::get_device();
    copy_commandlist = device->createCommandList();
}

Hd_RUZINO_Points::~Hd_RUZINO_Points()
{
}

void Hd_RUZINO_Points::refresh_debug_prims(Hd_RUZINO_RenderParam* render_param)
{
    std::vector<Hd_RUZINO_Points*> prims;
    {
        std::lock_guard lock(s_debug_prims_mutex);
        prims = s_debug_prims;
    }
    for (Hd_RUZINO_Points* prim : prims) {
        if (prim->debug_key.empty())
            continue;
        nvrhi::BufferHandle buf;
        size_t bytes = 0;
        uint64_t version = 0;
        const void* meta_ptr = nullptr;
        size_t meta_bytes = 0;
        bool hit = SharedGPUBufferRegistry::get().lookup(
            prim->debug_key, buf, bytes, version, &meta_ptr, &meta_bytes);
        if (!hit || !buf || meta_bytes < 4 * sizeof(uint32_t))
            continue;
        if (version == prim->debug_registry_version)
            continue;  // nothing new this frame

        struct DebugDrawMeta {
            uint32_t count;
            uint32_t capacity;
            uint32_t isSegments;
            uint32_t pad;
        };
        auto* meta = static_cast<const DebugDrawMeta*>(meta_ptr);
        prim->debug_registry_version = version;
        prim->debug_buffer = buf;
        prim->debug_count = std::min(meta->count, meta->capacity);
        prim->debug_capacity = std::max(meta->capacity, 1u);
        prim->debug_segments = meta->isSegments != 0;
        if (prim->debug_count == 0) {
            prim->BLAS = nullptr;
            prim->_pointsValid = false;
            continue;
        }
        // NOTE: no execution_launch_mutex here — create_gpu_resources_registry
        // and updateTLAS reach DeviceMemoryPool methods that take it
        // internally (non-recursive mutex → deadlock). This path runs on the
        // render thread; the only concurrent GPU-assembler users (Hydra
        // SyncAll workers calling compute_*_aabbs) serialize inside those
        // functions.
        prim->create_gpu_resources_registry(render_param);
        prim->_pointsValid = true;
        prim->updateTLAS(render_param, nullptr, nullptr);
        render_param->InstanceCollection->mark_geometry_dirty();
    }
}

HdDirtyBits Hd_RUZINO_Points::GetInitialDirtyBitsMask() const
{
    int mask = HdChangeTracker::Clean | HdChangeTracker::InitRepr |
               HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTransform |
               HdChangeTracker::DirtyVisibility |
               HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyWidths |
               HdChangeTracker::DirtyInstancer |
               HdChangeTracker::DirtyMaterialId;

    return (HdDirtyBits)mask;
}

HdDirtyBits Hd_RUZINO_Points::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void Hd_RUZINO_Points::create_gpu_resources(Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();

    if (!copy_commandlist)
        copy_commandlist = device->createCommandList(
            nvrhi::CommandListParameters{}.setQueueType(
                nvrhi::CommandQueue::Copy));

    // Calculate buffer layout: [positions][radii][colors]
    size_t position_buffer_offset = 0;
    size_t radius_buffer_offset = 0;
    size_t color_buffer_offset = 0;

    // Position buffer: 3 floats per point (x, y, z)
    size_t total_buffer_size = points.size() * 3 * sizeof(float);
    radius_buffer_offset = total_buffer_size;

    // Radius buffer: 1 float per point
    total_buffer_size += widths.size() * sizeof(float);
    color_buffer_offset = total_buffer_size;

    // Color block: 3 floats per point (only when displayColor is per-point)
    const bool has_colors = colors.size() == points.size();
    if (has_colors)
        total_buffer_size += colors.size() * 3 * sizeof(float);

    if (!vertexBuffer || vertexBuffer->getDesc().byteSize != total_buffer_size)

    {
        // Create vertex buffer for positions and radii
        nvrhi::BufferDesc desc =
            nvrhi::BufferDesc{}
                .setCanHaveRawViews(true)
                .setByteSize(total_buffer_size)
                .setIsVertexBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setCpuAccess(nvrhi::CpuAccessMode::None)
                .setIsAccelStructBuildInput(true)
                .setKeepInitialState(true)
                .setDebugName("sphereVertexBuffer");
        vertexBuffer = device->createBuffer(desc);
    }

    // Upload data to GPU
    copy_commandlist->open();

    // Write positions
    copy_commandlist->writeBuffer(
        vertexBuffer,
        points.data(),
        points.size() * 3 * sizeof(float),
        position_buffer_offset);

    // Write radii (widths are already radii, no conversion needed)
    copy_commandlist->writeBuffer(
        vertexBuffer,
        widths.data(),
        widths.size() * sizeof(float),
        radius_buffer_offset);

    // Write per-point colors (RGB block after the radii)
    if (has_colors) {
        copy_commandlist->writeBuffer(
            vertexBuffer,
            colors.data(),
            colors.size() * 3 * sizeof(float),
            color_buffer_offset);
    }

    copy_commandlist->close();

    {
        {
            std::lock_guard lock(execution_launch_mutex);
            device->executeCommandList(copy_commandlist);
        }

        // Create AABB buffer if not already created or size changed
        size_t required_aabb_size =
            points.size() * sizeof(nvrhi::rt::GeometryAABB);
        if (!aabbBuffer ||
            aabbBuffer->getDesc().byteSize != required_aabb_size) {
            nvrhi::BufferDesc aabb_desc =
                nvrhi::BufferDesc{}
                    .setByteSize(required_aabb_size)
                    .setStructStride(sizeof(nvrhi::rt::GeometryAABB))
                    .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                    .setKeepInitialState(true)
                    .setCanHaveUAVs(true)
                    .setIsAccelStructBuildInput(true)
                    .setDebugName("sphere_aabbs");
            aabbBuffer = device->createBuffer(aabb_desc);
        }

        // Use GPU to compute AABBs from sphere positions and radii
        GPUSceneAssember::compute_sphere_aabbs(
            vertexBuffer,
            position_buffer_offset,
            radius_buffer_offset,
            points.size(),
            aabbBuffer);

        // Build BLAS for spheres using AABBs
        nvrhi::rt::AccelStructDesc blas_desc;
        nvrhi::rt::GeometryDesc geometry_desc;
        geometry_desc.geometryType = nvrhi::rt::GeometryType::AABBs;
        geometry_desc.useTransform = false;

        nvrhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.setBuffer(aabbBuffer)
            .setCount(points.size())
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

        auto descriptor_table =
            render_param->InstanceCollection->get_buffer_descriptor_table();
        descriptor_handle = descriptor_table->CreateDescriptorHandle(
            nvrhi::BindingSetItem::RawBuffer_SRV(0, vertexBuffer));
    }

    // Create mesh descriptor for sphere vertex buffer
    const SdfPath& id = GetId();
    MeshDesc mesh_desc;
    mesh_desc.vbOffset = position_buffer_offset;
    mesh_desc.bindlessIndex = descriptor_handle.Get();
    mesh_desc.ibOffset =
        radius_buffer_offset;  // Store radius buffer offset in ibOffset
    // Per-point color block lives at texCrdOffset when displayColor was
    // per-point; hit groups that see HasPerPointColor shade with it directly.
    mesh_desc.texCrdOffset = has_colors ? uint32_t(color_buffer_offset) : 0u;
    mesh_desc.normalOffset = 0;
    mesh_desc.tangentOffset = 0;
    mesh_desc.subsetMatIdOffset = 0;
    mesh_desc.flags = has_colors ? uint32_t(MeshFlags::HasPerPointColor)
                                 : uint32_t(MeshFlags::None);

    if (!mesh_desc_buffer)
        mesh_desc_buffer =
            render_param->InstanceCollection->mesh_pool.allocate(1);
    mesh_desc_buffer->write_data(&mesh_desc);

    spdlog::info(
        "Points {}: created mesh descriptor at index {}",
        id.GetText(),
        mesh_desc_buffer->index());

    spdlog::info("Created sphere BLAS with {} points", points.size());
}

// Debug zero-copy registry mode: build AABBs + BLAS straight from the shared
// GPU buffer packed by brush_wb_commit (no upload). Rebuilt every registry
// version bump — unlike the volume rprim, whose AABB slab is static, point
// positions move every frame.
void Hd_RUZINO_Points::create_gpu_resources_registry(
    Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();

    if (!copy_commandlist)
        copy_commandlist = device->createCommandList();

    const size_t C = debug_capacity;
    const size_t pos_off = 0;
    const size_t endb_off = 12 * C;  // segments: endpoint B block
    const size_t radius_off = debug_segments ? 24 * C : 12 * C;
    const size_t color_off = debug_segments ? 28 * C : 16 * C;

    // AABB buffer sized for CAPACITY (live count changes frame to frame; the
    // BLAS below uses only the first debug_count entries).
    size_t required_aabb_size = C * sizeof(nvrhi::rt::GeometryAABB);
    if (!aabbBuffer || aabbBuffer->getDesc().byteSize != required_aabb_size) {
        nvrhi::BufferDesc aabb_desc =
            nvrhi::BufferDesc{}
                .setByteSize(required_aabb_size)
                .setStructStride(sizeof(nvrhi::rt::GeometryAABB))
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveUAVs(true)
                .setDebugName("debug_points_aabbs");
        aabbBuffer = device->createBuffer(aabb_desc);
    }

    if (debug_segments)
        GPUSceneAssember::compute_segment_aabbs(
            debug_buffer,
            pos_off,
            endb_off,
            radius_off,
            debug_count,
            aabbBuffer);
    else
        GPUSceneAssember::compute_sphere_aabbs(
            debug_buffer, pos_off, radius_off, debug_count, aabbBuffer);

    // Release the previous frame's BLAS before building the new one (the
    // waitForIdle below guarantees no in-flight rays reference it, and the
    // TLAS is rebuilt before the next trace via set_require_rebuild_tlas).
    BLAS = nullptr;

    nvrhi::rt::AccelStructDesc blas_desc;
    nvrhi::rt::GeometryDesc geometry_desc;
    geometry_desc.geometryType = nvrhi::rt::GeometryType::AABBs;
    geometry_desc.useTransform = false;

    nvrhi::rt::GeometryAABBs aabbGeometry;
    aabbGeometry.setBuffer(aabbBuffer)
        .setCount(debug_count)
        .setStride(sizeof(nvrhi::rt::GeometryAABB))
        .setOffset(0);

    geometry_desc.setAABBs(aabbGeometry);
    blas_desc.addBottomLevelGeometry(geometry_desc);
    blas_desc.isTopLevel = false;

    BLAS = device->createAccelStruct(blas_desc);
    if (!command_list)
        command_list = device->createCommandList();
    command_list->open();
    nvrhi::utils::BuildBottomLevelAccelStruct(command_list, BLAS, blas_desc);
    command_list->close();
    device->executeCommandList(command_list);
    device->waitForIdle();

    // One bindless descriptor per buffer handle (the handle is stable across
    // frames, so this is created once; content updates flow through it).
    if (!descriptor_handle.Get() ||
        debug_descriptor_buffer != debug_buffer.Get()) {
        auto descriptor_table =
            render_param->InstanceCollection->get_buffer_descriptor_table();
        descriptor_handle = descriptor_table->CreateDescriptorHandle(
            nvrhi::BindingSetItem::RawBuffer_SRV(0, debug_buffer));
        debug_descriptor_buffer = debug_buffer.Get();
    }

    MeshDesc mesh_desc;
    mesh_desc.vbOffset = uint32_t(pos_off);
    mesh_desc.bindlessIndex = descriptor_handle.Get();
    mesh_desc.ibOffset = uint32_t(radius_off);
    mesh_desc.texCrdOffset = uint32_t(color_off);
    mesh_desc.normalOffset = uint32_t(debug_segments ? endb_off : 0);
    mesh_desc.tangentOffset = 0;
    mesh_desc.subsetMatIdOffset = 0;
    mesh_desc.flags = uint32_t(MeshFlags::HasPerPointColor) |
                      (debug_segments ? uint32_t(MeshFlags::IsCapsuleSegments)
                                      : uint32_t(MeshFlags::None));

    if (!mesh_desc_buffer)
        mesh_desc_buffer =
            render_param->InstanceCollection->mesh_pool.allocate(1);
    mesh_desc_buffer->write_data(&mesh_desc);

    spdlog::info(
        "Points {} [debug:{}]: {} {} (capacity {}, bindless={})",
        GetId().GetText(),
        debug_key,
        debug_count,
        debug_segments ? "segments" : "points",
        debug_capacity,
        mesh_desc.bindlessIndex);
}

void Hd_RUZINO_Points::updateTLAS(
    Hd_RUZINO_RenderParam* render_param,
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits* dirtyBits)
{
    // sceneDelegate may be null when called from the render-thread refresh
    // path (refresh_debug_prims) — debug prims are never instanced, so the
    // instancer queries are skippable.
    if (sceneDelegate) {
        _UpdateInstancer(sceneDelegate, dirtyBits);
        HdInstancer::_SyncInstancerAndParents(
            sceneDelegate->GetRenderIndex(), GetInstancerId());
    }
    const SdfPath& id = GetId();

    auto material_id = GetMaterialId();

    if (material_id.IsEmpty()) {
        spdlog::warn("Points {} has no material assigned", id.GetText());
    }

    Hd_RUZINO_Material* material = (*render_param->material_map)[material_id];
    if (!material) {
        spdlog::warn(
            "Material {} not found for points {}. Continuing without material.",
            material_id.GetText(),
            id.GetText());
    }

    size_t instance_count = 1;

    // Determine instance count
    if (sceneDelegate && !GetInstancerId().IsEmpty()) {
        HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer* instancer = renderIndex.GetInstancer(GetInstancerId());
        VtIntArray instanceIndices =
            sceneDelegate->GetInstanceIndices(GetInstancerId(), GetId());
        instance_count = instanceIndices.size();
        spdlog::info(
            "Points {} has instancer {} with {} instances",
            id.GetText(),
            GetInstancerId().GetText(),
            instance_count);
    }
    else {
        spdlog::info(
            "Points {} has no instancer, using single instance", id.GetText());
    }

    auto& rt_instance_pool = render_param->InstanceCollection->rt_instance_pool;

    if (!rt_instanceBuffer || rt_instanceBuffer->count() != instance_count)
        rt_instanceBuffer = rt_instance_pool.allocate(instance_count);
    if (!instanceBuffer || instanceBuffer->count() != instance_count)
        instanceBuffer =
            render_param->InstanceCollection->instance_pool.allocate(
                instance_count);

    if (material) {
        material->ensure_material_data_handle(render_param);
    }

    // CPU path: Single instance or manual instancing
    GeometryInstanceData instance_data;
    instance_data.geometryID =
        mesh_desc_buffer->index();  // Use sphere mesh descriptor
    instance_data.materialID = material ? material->GetMaterialLocation() : -1;
    memcpy(&instance_data.transform, transform.data(), sizeof(pxr::GfMatrix4f));
    instance_data.flags = 0;

    instanceBuffer->write_data(&instance_data);

    nvrhi::rt::InstanceDesc rt_instance;
    rt_instance.blasDeviceAddress = BLAS->getDeviceAddress();
    rt_instance.instanceMask = 1;
    rt_instance.instanceContributionToHitGroupIndex =
        debug_segments ? 6 : 2;  // sphere hit groups (2/3) or capsule (6/7)
    rt_instance.flags = nvrhi::rt::InstanceFlags::None;

    GfMatrix4f mat_transposed = transform.GetTranspose();
    memcpy(
        rt_instance.transform,
        mat_transposed.data(),
        sizeof(nvrhi::rt::AffineTransform));
    rt_instance.instanceID = instanceBuffer->index();

    rt_instanceBuffer->write_data(&rt_instance);

    render_param->InstanceCollection->set_require_rebuild_tlas();

    spdlog::info(
        "Updated TLAS for points {} with {} instances",
        id.GetText(),
        instance_count);
}

void Hd_RUZINO_Points::_InitRepr(
    const TfToken& reprToken,
    HdDirtyBits* dirtyBits)
{
}

void Hd_RUZINO_Points::Sync(
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

    // Handle transform
    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }

    // Handle material (both paths — the registry branch returns early below)
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        SdfPath const& newMaterialId = sceneDelegate->GetMaterialId(id);
        if (GetMaterialId() != newMaterialId) {
            SetMaterialId(newMaterialId);
            spdlog::info(
                "Points {}: material updated to {}",
                id.GetText(),
                newMaterialId.GetText());
        }
    }

    // The "debugKey" primvar selects zero-copy registry mode: the prim renders
    // a SharedGPUBufferRegistry buffer packed by the simulation (see
    // node_brush_wb_commit's debug-draw pack) instead of USD point data.
    if (*dirtyBits &
        (HdChangeTracker::DirtyPrimvar | HdChangeTracker::InitRepr)) {
        VtValue key_v;
        if (!Ruzino_Hydra2::ReadPrimvar(
                sceneDelegate, id, TfToken("debugKey"), &key_v)) {
            key_v = sceneDelegate->Get(id, TfToken("debugKey"));
        }
        std::string key;
        if (!key_v.IsEmpty()) {
            if (key_v.IsHolding<std::string>())
                key = key_v.UncheckedGet<std::string>();
            else if (key_v.IsHolding<TfToken>())
                key = key_v.UncheckedGet<TfToken>().GetString();
        }
        if (!key.empty() && key != debug_key) {
            debug_key = key;
            debug_registry_version =
                0;  // force a rebuild on the next registry hit
        }
    }

    if (!debug_key.empty()) {
        // ---- registry mode --------------------------------------------
        // Per-frame rebuilds are driven by refresh_debug_prims from the
        // render-thread callback (Hd_RUZINO_Renderer::Render polls the
        // registry), NOT by Hydra dirty propagation — see the note on
        // refresh_debug_prims. Sync only registers the prim for refreshing.
        {
            std::lock_guard lock(s_debug_prims_mutex);
            if (std::find(s_debug_prims.begin(), s_debug_prims.end(), this) ==
                s_debug_prims.end())
                s_debug_prims.push_back(this);
        }
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // ---- regular USD points path ----------------------------------------
    // Handle points data
    if (*dirtyBits & HdChangeTracker::DirtyPoints) {
        VtValue pointsValue;
        if (!Ruzino_Hydra2::ReadPrimvar(
                sceneDelegate, id, HdTokens->points, &pointsValue)) {
            pointsValue = sceneDelegate->Get(id, HdTokens->points);
        }
        if (pointsValue.IsHolding<VtArray<GfVec3f>>()) {
            points = pointsValue.UncheckedGet<VtArray<GfVec3f>>();
            _pointsValid = true;
            update_gpu_resources = true;

            spdlog::info(
                "Points {}: loaded {} points", id.GetText(), points.size());
        }
        else if (!pointsValue.IsEmpty()) {
            spdlog::warn(
                "Points {}: points primvar holds {} (expected point3f[]), "
                "keeping previous points",
                id.GetText(),
                pointsValue.GetTypeName());
        }
    }

    // Handle widths/radii
    if (*dirtyBits & HdChangeTracker::DirtyWidths) {
        VtValue widthsValue;
        if (!Ruzino_Hydra2::ReadPrimvar(
                sceneDelegate, id, HdTokens->widths, &widthsValue)) {
            widthsValue = sceneDelegate->Get(id, HdTokens->widths);
        }
        if (!widthsValue.IsEmpty() && widthsValue.IsHolding<VtFloatArray>()) {
            widths = widthsValue.UncheckedGet<VtFloatArray>();
        }
        else if (!widthsValue.IsEmpty()) {
            spdlog::warn(
                "Points {}: widths primvar holds {} (expected float[]), "
                "using default widths",
                id.GetText(),
                widthsValue.GetTypeName());
            widths.resize(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                widths[i] = 0.1f;  // Default radius
            }
        }
        else {
            // Default width if not specified
            widths.resize(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                widths[i] = 0.1f;  // Default radius
            }
        }
        update_gpu_resources = true;

        spdlog::info(
            "Points {}: loaded {} widths", id.GetText(), widths.size());
    }

    // Handle per-point displayColor (vertex interpolation).
    if (*dirtyBits & HdChangeTracker::DirtyPrimvar) {
        VtValue col_v;
        if (!Ruzino_Hydra2::ReadPrimvar(
                sceneDelegate, id, HdTokens->displayColor, &col_v)) {
            col_v = sceneDelegate->Get(id, HdTokens->displayColor);
        }
        if (!col_v.IsEmpty() && col_v.IsHolding<VtVec3fArray>()) {
            VtVec3fArray cols = col_v.UncheckedGet<VtVec3fArray>();
            if (cols.size() == points.size()) {
                colors = std::move(cols);
                update_gpu_resources = true;
            }
        }
    }

    // Create or update GPU resources
    if (update_gpu_resources && _pointsValid && !points.empty()) {
        create_gpu_resources(render_param);
    }

    // Update TLAS
    if (_pointsValid && BLAS) {
        updateTLAS(render_param, sceneDelegate, dirtyBits);
    }
    static_cast<Hd_RUZINO_RenderParam*>(renderParam)
        ->InstanceCollection->mark_geometry_dirty();

    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_Points::Finalize(HdRenderParam* renderParam)
{
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    {
        std::lock_guard lock(s_debug_prims_mutex);
        s_debug_prims.erase(
            std::remove(s_debug_prims.begin(), s_debug_prims.end(), this),
            s_debug_prims.end());
    }

    if (instanceBuffer)
        instanceBuffer.reset();
    if (rt_instanceBuffer)
        rt_instanceBuffer.reset();
    if (mesh_desc_buffer)
        mesh_desc_buffer.reset();

    spdlog::info("Finalized points {}", GetId().GetText());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
