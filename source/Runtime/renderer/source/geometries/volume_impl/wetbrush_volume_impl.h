// Wetbrush paint-slab volume implementation.
//
// Renders the Wetbrush 2D/3D paint layer as a raymarchable density slab. The
// density+color data is a Float4 (density, r, g, b) buffer, either:
//   - zero-copy adopted from the simulation's SharedGPUBufferRegistry (key
//     "wetbrush_paint_field") when the sim registers a packed buffer, or
//   - created from the `paintField` primvar (bake fallback).
//
// The grid AABB = gridMin + cellSize*res per axis; one procedural AABB covers
// the whole slab. Routes to hit-group slot 4 (VolumeClosestHit/VolumeShadowHit
// in wetbrush_render.slang). See render_wetbrush.py bake stage for the primvar
// authoring.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume (the non-cloud half).
#pragma once

#include <vector>

#include "DescriptorTableManager.h"  // DescriptorHandle
#include "pxr/base/gf/vec4f.h"
#include "volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Wetbrush paint-slab volume strategy.
class WetbrushVolumeImpl : public VolumeImpl {
   public:
    VolumeKind kind() const override
    {
        return VolumeKind::Wetbrush;
    }
    uint32_t hitGroupIndex() const override
    {
        return 4;
    }
    std::string debugName() const override
    {
        return "wetbrushVolumeAABB";
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
    // 3D grid metadata (pulled from primvars).
    uint32_t gridResX = 0;
    uint32_t gridResY = 0;
    uint32_t gridResZ = 0;
    float cellSize = 0.0f;
    GfVec3f gridMin = GfVec3f(0.0f);
    // Per-voxel paint data (gridResX*gridResY*gridResZ Float4: density,r,g,b).
    // Only used when falling back to the primvar path (registry miss).
    std::vector<GfVec4f> paintField;

    // Bindless (density, r, g, b) Float4 buffer, one entry per canvas cell.
    nvrhi::BufferHandle densityBuffer;
    DescriptorHandle densityDescriptorHandle;

    // Shared-registry two-phase lookup state. When the sim registers a packed
    // Float4 buffer under "wetbrush_paint_field", we use it directly (zero
    // copy) instead of creating our own buffer from the primvar data.
    uint64_t registryVersion = 0;

    // Bindless SRV index populated by buildDensityResource; read by
    // fillVolumeDesc.
    uint32_t bindlessIndex_ = 0;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
