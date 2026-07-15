//
// Hd_RUZINO_WetbrushVolume — a raymarchable density-slab rprim for rendering
// the Wetbrush paint layer with the path tracer's volume hit groups.
//
// Unlike Hd_RUZINO_Volume (which loads OpenVDB files but never renders), this
// rprim carries a flat 2D canvas layer (gridRes x gridRes cells of
// (density, r, g, b)) plus the slab world-AABB, and registers itself into the
// TLAS as a procedural-AABB BLAS routed to hit-group slots 4/5
// (VolumeClosestHit / VolumeShadowHit with VolumeIntersection). The shaders do
// the raymarch.
//
// The density+color data is authored on the USD prim as a Float4Array primvar
// named "paintField" with `vertex` interpolation (one entry per canvas cell),
// plus standard USD Volume field dims via the "gridRes" / "gridPaper" /
// "gridCenter" / "canvasFloorZ" primvars. See render_wetbrush.py bake stage.
//
// Mirrors Hd_RUZINO_Points' BLAS/TLAS pattern (points.cpp:108-156, 233-272)
// but with a single CPU-computed AABB covering the whole slab (no GPU compute
// pass) and instanceContributionToHitGroupIndex = 4.
//
#ifndef Hd_RUZINO_WETBRUSH_VOLUME_H
#define Hd_RUZINO_WETBRUSH_VOLUME_H

#include <vector>

#include "../DescriptorTableManager.h"
#include "../api.h"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/volume.h"
#include "pxr/pxr.h"
// SceneTypes (VolumeDesc, GeometryInstanceData)
#include "../nodes/shaders/shaders/Scene/SceneTypes.slang"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_RenderParam;
using namespace pxr;

class HD_RUZINO_API Hd_RUZINO_WetbrushVolume final : public HdVolume {
   public:
    HF_MALLOC_TAG_NEW("new Hd_RUZINO_WetbrushVolume");

    Hd_RUZINO_WetbrushVolume(const SdfPath& id);
    ~Hd_RUZINO_WetbrushVolume() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits,
        const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

    nvrhi::rt::AccelStructHandle BLAS;
    CommandListHandle command_list;

   protected:
    // Bindless (density, r, g, b) Float4 buffer, one entry per canvas cell.
    nvrhi::BufferHandle densityBuffer;
    DescriptorHandle densityDescriptorHandle;

    // Single procedural AABB covering the whole slab (CPU-computed).
    nvrhi::BufferHandle aabbBuffer;

    CommandListHandle copy_commandlist;

    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle instanceBuffer;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle rt_instanceBuffer;
    DeviceMemoryPool<VolumeDesc>::MemoryHandle volume_desc_buffer;

    GfMatrix4f transform;

    // 3D grid metadata (pulled from primvars).
    uint32_t gridResX = 0;
    uint32_t gridResY = 0;
    uint32_t gridResZ = 0;
    float cellSize = 0.0f;
    GfVec3f gridMin = GfVec3f(0.0f);
    // Per-voxel paint data (gridResX*gridResY*gridResZ Float4: density,r,g,b).
    // Only used when falling back to the primvar path (registry miss).
    std::vector<GfVec4f> paintField;
    bool _valid = false;

    // Shared-registry two-phase lookup state. When the sim registers a packed
    // Float4 buffer under "wetbrush_paint_field", we use it directly (zero copy)
    // instead of creating our own buffer from the primvar data.
    uint64_t registryVersion = 0;   // last-seen registry version for the buffer

    void create_gpu_resources(Hd_RUZINO_RenderParam* render_param);
    void updateTLAS(
        Hd_RUZINO_RenderParam* render_param,
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits* dirtyBits);

    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

    Hd_RUZINO_WetbrushVolume(const Hd_RUZINO_WetbrushVolume&) = delete;
    Hd_RUZINO_WetbrushVolume& operator=(const Hd_RUZINO_WetbrushVolume&) = delete;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_WETBRUSH_VOLUME_H
