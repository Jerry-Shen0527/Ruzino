// Procedural path-traced cloud volume implementation.
//
// Renders a cloud as a UsdVol.Volume with no stored density grid: density is
// generated on the GPU by cloud_intersection.slang from fbm+Worley noise. Only
// the world AABB (for the procedural BLAS + the shader's slab test) and a
// cloud-flavoured VolumeDesc carrying the shaping parameters are needed. Routes
// to hit-group slot 6 (CloudClosestHit/CloudShadowHit in path_tracing.slang).
//
// Extracted from the old Hd_RUZINO_WetbrushVolume cloud branch.
#pragma once

#include "volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Procedural cloud volume strategy. No stored grid; density is GPU-generated.
class CloudVolumeImpl : public VolumeImpl {
   public:
    VolumeKind kind() const override
    {
        return VolumeKind::Cloud;
    }
    uint32_t hitGroupIndex() const override
    {
        return 6;
    }
    std::string debugName() const override
    {
        return "cloudVolumeAABB";
    }

    GfVec3f boundsMin() const override
    {
        return cloud_bounds_min;
    }
    GfVec3f boundsMax() const override
    {
        return cloud_bounds_max;
    }

    bool parsePrimvars(
        HdSceneDelegate* sceneDelegate,
        const SdfPath& id,
        HdDirtyBits* dirtyBits) override;

    // Clouds have no density buffer — density is procedural on the GPU.
    bool buildDensityResource(Hd_RUZINO_RenderParam* /*render_param*/) override
    {
        return true;
    }

    void fillVolumeDesc(VolumeDesc& vd) const override;

   private:
    GfVec3f cloud_bounds_min = GfVec3f(-50.0f, 0.0f, -50.0f);
    GfVec3f cloud_bounds_max = GfVec3f(50.0f, 20.0f, 50.0f);
    float cloud_coverage = 0.35f;
    float cloud_densityScale = 1.2f;
    float cloud_phaseG = 0.7f;
    float cloud_layerTop = 1.0f;
    float cloud_layerBottom = 0.0f;
    float cloud_noiseFreq = 4.0f;
    float cloud_worleyFreq = 4.0f;
    float cloud_detailErosion = 0.7f;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
