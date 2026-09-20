// Generic voxel grid volume implementation (VolumeKind::GridVolume).
//
// Renders a per-voxel (density, r, g, b) field as a raymarchable slab with
// the Wetbrush paper §6 paint shading (first-cross + penetration blend + AO).
// The data is a Float4 buffer, either:
//   - zero-copy adopted from a simulation's SharedGPUBufferRegistry entry
//     (a "grid_field/" key whose metadata blob is a Ruzino::GridFieldDesc,
//     see RHI/grid_field_desc.hpp — e.g. the wetbrush paint field under
//     kGridFieldPaintKey), or
//   - created from the `paintField` primvar (bake fallback).
//
// The grid AABB = origin + spacing*res per axis; one procedural AABB covers
// the whole slab. Routes to hit-group slots 4/5 (VolumeClosestHit /
// VolumeShadowHit + VolumeIntersection in path_tracing.slang). See
// render_wetbrush.py bake stage for the primvar authoring.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume (the non-cloud half);
// generalized from the wetbrush-private PaintFieldMeta contract to the
// shared GridFieldDesc header.
#pragma once

#include <vector>

#include "DescriptorTableManager.h"  // DescriptorHandle
#include "pxr/base/gf/vec4f.h"
#include "volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Generic voxel grid volume strategy (paint density+color slab).
class GridVolumeImpl : public VolumeImpl {
   public:
    VolumeKind kind() const override
    {
        return VolumeKind::GridVolume;
    }
    uint32_t hitGroupIndex() const override
    {
        return 4;
    }
    std::string debugName() const override
    {
        return "gridVolumeAABB";
    }

    GfVec3f boundsMin() const override;
    GfVec3f boundsMax() const override;

    bool parsePrimvars(
        HdSceneDelegate* sceneDelegate,
        const SdfPath& id,
        HdDirtyBits* dirtyBits) override;

    bool buildDensityResource(Hd_RUZINO_RenderParam* render_param) override;

    void fillVolumeDesc(VolumeDesc& vd) const override;

   private:
    // 3D grid metadata (pulled from the shared GridFieldDesc registry blob,
    // or the grid primvars on the bake fallback path).
    uint32_t gridResX = 0;
    uint32_t gridResY = 0;
    uint32_t gridResZ = 0;
    float spacingX = 0.0f;
    float spacingY = 0.0f;
    float spacingZ = 0.0f;
    GfVec3f gridMin = GfVec3f(0.0f);
    // Per-voxel paint data (gridResX*gridResY*gridResZ Float4: density,r,g,b).
    // Only used when falling back to the primvar path (registry miss).
    std::vector<GfVec4f> paintField;

    // Bindless (density, r, g, b) Float4 buffer, one entry per grid cell.
    nvrhi::BufferHandle densityBuffer;
    DescriptorHandle densityDescriptorHandle;

    // Shared-registry two-phase lookup state. When a producer registers a
    // packed Float4 buffer under kGridFieldPaintKey, we use it directly
    // (zero copy) instead of creating our own buffer from the primvar data.
    uint64_t registryVersion = 0;

    // Bindless SRV index populated by buildDensityResource; read by
    // fillVolumeDesc.
    uint32_t bindlessIndex_ = 0;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
