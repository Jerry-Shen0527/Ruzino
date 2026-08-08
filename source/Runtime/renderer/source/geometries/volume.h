//
// Hd_RUZINO_Volume — the single Hydra rprim for the `volume` token.
//
// This is the unified volume rprim: a thin shell (pimpl) that owns the shared
// rendering machinery (AABB/BLAS, pool handles, TLAS instance writing) and
// delegates the type-specific behaviour to a VolumeImpl strategy, selected at
// Sync time from the `volumeType` primvar:
//
//   volumeType == "cloud"  -> CloudVolumeImpl     (procedural path-traced
//   cloud) absent / "wetbrush"    -> WetbrushVolumeImpl  (paint density slab)
//
// The shell never branches on volume type itself — it calls
// impl_->hitGroupIndex() / fillVolumeDesc() / etc. Adding a new volume type
// (e.g. file-based VDB) is a new VolumeImpl subclass in its own file + a case
// in VolumeImpl::resolve(); no change to this shell or the TLAS code.
//
// This is the pimpl + strategy pattern (composition, private inheritance
// expressed as a pointer): one stable Hydra interface, multiple independent
// implementation files under volume_impl/. The old Hd_RUZINO_WetbrushVolume's
// `is_cloud` flag is gone — wetbrush/cloud are two implementations, not two
// branches of one class.
//
// CreateRprim always constructs this class (mirrors the material pattern, where
// CreateSprim always constructs Hd_RUZINO_MaterialX). The volumeType primvar is
// only readable from Sync (CreateRprim has no sceneDelegate), which is why the
// type selection happens in Sync, not at creation.
//
#ifndef Hd_RUZINO_VOLUME_H
#define Hd_RUZINO_VOLUME_H

#include "../api.h"
#include "DescriptorTableManager.h"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/volume.h"
#include "pxr/pxr.h"
// SceneTypes (VolumeDesc, GeometryInstanceData, GeometryType) — shared host/
// device header, the same include used by renderTLAS.h / mesh.h.
#include <memory>

#include "../nodes/shaders/Scene/SceneTypes.slang"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_RenderParam;
class VolumeImpl;
using namespace pxr;

class HD_RUZINO_API Hd_RUZINO_Volume final : public HdVolume {
   public:
    HF_MALLOC_TAG_NEW("new Hd_RUZINO_Volume");

    Hd_RUZINO_Volume(const SdfPath& id);
    // Destructor defined in the .cpp so the forward-declared
    // unique_ptr<VolumeImpl> can be destroyed with a complete type (standard
    // pimpl requirement).
    ~Hd_RUZINO_Volume() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits,
        const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

    // The TLAS build reads BLAS->getDeviceAddress(); keep it public like the
    // old Hd_RUZINO_WetbrushVolume.
    nvrhi::rt::AccelStructHandle BLAS;

   private:
    // --- Shared rendering machinery (all volume types) ---
    nvrhi::BufferHandle aabbBuffer;      // single procedural AABB
    CommandListHandle copy_commandlist;  // for CPU→GPU uploads
    CommandListHandle command_list;      // for the BLAS build

    // Pool entries (indices feed the TLAS / shader descriptor tables).
    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle instanceBuffer;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle rt_instanceBuffer;
    DeviceMemoryPool<VolumeDesc>::MemoryHandle volume_desc_buffer;

    GfMatrix4f transform;
    bool _valid = false;

    // --- pimpl: the type-specific strategy (wetbrush / cloud / ...) ---
    std::unique_ptr<VolumeImpl> impl_;

    // Shared helpers.
    void create_gpu_resources(Hd_RUZINO_RenderParam* render_param);
    void updateTLAS(
        Hd_RUZINO_RenderParam* render_param,
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits* dirtyBits);

    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

    Hd_RUZINO_Volume(const Hd_RUZINO_Volume&) = delete;
    Hd_RUZINO_Volume& operator=(const Hd_RUZINO_Volume&) = delete;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_VOLUME_H
