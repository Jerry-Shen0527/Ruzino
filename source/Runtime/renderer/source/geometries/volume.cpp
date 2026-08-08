//
// Hd_RUZINO_Volume implementation (the pimpl shell).
//
// Owns the shared rendering machinery (AABB/BLAS build, pool-handle allocation,
// TLAS instance writing) and delegates the type-specific behaviour to a
// VolumeImpl strategy. See volume.h for the architecture.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume: the wetbrush-specific logic
// moved to volume_impl/wetbrush_volume_impl.cpp, the cloud logic to
// volume_impl/cloud_volume_impl.cpp, and the duplicated AABB/BLAS block to
// volume_impl/volume_impl.cpp::buildSingleAabbBlas. What remains here is the
// type-agnostic orchestration.
#include "volume.h"

#include <spdlog/spdlog.h>

#include "../instancer.h"
#include "../renderParam.h"
#include "material/material.h"
#include "nvrhi/utils.h"
#include "pxr/imaging/hd/instancer.h"
#include "volume_impl/volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Volume::Hd_RUZINO_Volume(const SdfPath& id) : HdVolume(id)
{
    auto device = RHI::get_device();
    copy_commandlist = device->createCommandList();
}

// Defined here (not = default in the header) so ~unique_ptr<VolumeImpl> sees a
// complete VolumeImpl type.
Hd_RUZINO_Volume::~Hd_RUZINO_Volume() = default;

HdDirtyBits Hd_RUZINO_Volume::GetInitialDirtyBitsMask() const
{
    int mask = HdChangeTracker::Clean | HdChangeTracker::InitRepr |
               HdChangeTracker::DirtyTransform |
               HdChangeTracker::DirtyVisibility |
               HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyInstancer |
               HdChangeTracker::DirtyMaterialId;
    return (HdDirtyBits)mask;
}

HdDirtyBits Hd_RUZINO_Volume::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void Hd_RUZINO_Volume::_InitRepr(
    const TfToken& reprToken,
    HdDirtyBits* dirtyBits)
{
}

void Hd_RUZINO_Volume::create_gpu_resources(Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();
    if (!copy_commandlist)
        copy_commandlist = device->createCommandList();

    // --- Shared: AABB buffer + upload + BLAS (the helper dedupes the ~45
    // lines that were duplicated between the old cloud and wetbrush branches).
    buildSingleAabbBlas(
        device,
        impl_->boundsMin(),
        impl_->boundsMax(),
        aabbBuffer,
        BLAS,
        command_list,
        copy_commandlist,
        impl_->debugName());

    // --- Shared: VolumeDesc pool entry. The shell fills boundsMin/boundsMax
    // (common to all volume types); the impl fills the type-specific fields.
    if (!volume_desc_buffer)
        volume_desc_buffer =
            render_param->InstanceCollection->volume_pool.allocate(1);

    VolumeDesc vd{};
    const GfVec3f bmin = impl_->boundsMin();
    const GfVec3f bmax = impl_->boundsMax();
    vd.boundsMin = float3(bmin[0], bmin[1], bmin[2]);
    vd.boundsMax = float3(bmax[0], bmax[1], bmax[2]);
    impl_->fillVolumeDesc(vd);
    volume_desc_buffer->write_data(&vd);

    _valid = true;
}

void Hd_RUZINO_Volume::updateTLAS(
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
    instance_data.flags = static_cast<uint32_t>(GeometryType::Custom)
                          << GeometryInstanceData::kTypeOffset;
    instanceBuffer->write_data(&instance_data);

    nvrhi::rt::InstanceDesc rt_instance;
    rt_instance.blasDeviceAddress = BLAS->getDeviceAddress();
    rt_instance.instanceMask = 1;
    // Hit-group slot is the ONLY type-dependent field; read it from the impl.
    // (wetbrush=4 → VolumeClosestHit/VolumeShadowHit; cloud=6 → Cloud*.)
    rt_instance.instanceContributionToHitGroupIndex = impl_->hitGroupIndex();
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

void Hd_RUZINO_Volume::Sync(
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

    // Pull the type-specific primvars. First resolve/select the impl from the
    // `volumeType` primvar (cloud vs wetbrush), then let it parse its own
    // primvars. This replaces the old `if (is_cloud) { ... } else { ... }`
    // split — the shell no longer knows which type it is.
    if (*dirtyBits &
        (HdChangeTracker::DirtyPrimvar | HdChangeTracker::InitRepr)) {
        // Select or keep the impl based on volumeType. On a type switch the old
        // impl is released; cached primvars/registry versions survive when the
        // type is unchanged.
        VolumeImpl::resolve(sceneDelegate, id, impl_);
        update_gpu_resources =
            impl_->parsePrimvars(sceneDelegate, id, dirtyBits);
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        SdfPath const& newMaterialId = sceneDelegate->GetMaterialId(id);
        if (GetMaterialId() != newMaterialId) {
            SetMaterialId(newMaterialId);
        }
    }

    // Build/refresh GPU resources when an impl asked for it. The impl builds
    // its own density/texture resource first (it may report the resources are
    // invalid — e.g. wetbrush with no registry buffer and no paint primvar — in
    // which case we skip the TLAS registration for this frame).
    if (update_gpu_resources && impl_) {
        bool density_ok = impl_->buildDensityResource(render_param);
        if (density_ok)
            create_gpu_resources(render_param);
        else
            _valid = false;
    }

    if (_valid && BLAS) {
        updateTLAS(render_param, sceneDelegate, dirtyBits);
    }
    render_param->InstanceCollection->mark_geometry_dirty();

    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_Volume::Finalize(HdRenderParam* renderParam)
{
    Hd_RUZINO_RenderParam* render_param =
        static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    if (instanceBuffer)
        instanceBuffer.reset();
    if (rt_instanceBuffer)
        rt_instanceBuffer.reset();
    if (volume_desc_buffer)
        volume_desc_buffer.reset();

    spdlog::info("Finalized Volume {}", GetId().GetText());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
