// Procedural path-traced cloud volume implementation.
//
// Renders a cloud as a UsdVol.Volume with no stored density grid: density is
// generated on the GPU by cloud_intersection.slang from fbm+Worley noise. Only
// the world AABB (for the procedural BLAS + the shader's slab test) and a
// cloud-flavoured VolumeDesc carrying the shaping parameters are needed.
//
// Two variants, selected by the `volumeType` primvar in VolumeImpl::resolve:
//   - biased   ("cloud",          unbiased=false): hit-group slots 6/7 —
//              fixed-step Beer-Lambert quadrature + heuristic multi-scatter;
//   - unbiased ("cloud_unbiased", unbiased=true):  hit-group slots 8/9 —
//              delta tracking / ratio tracking (cloud_unbiased.slang).
// Both share the same primvar parsing, VolumeDesc layout and density model;
// only the SBT routing and the debug name differ.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume cloud branch.
#pragma once

#include "volume_impl.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Procedural cloud volume strategy. No stored grid; density is GPU-generated.
class CloudVolumeImpl : public VolumeImpl {
   public:
    explicit CloudVolumeImpl(bool unbiased = false) : cloud_unbiased_(unbiased)
    {
    }

    VolumeKind kind() const override
    {
        return cloud_unbiased_ ? VolumeKind::CloudUnbiased : VolumeKind::Cloud;
    }
    uint32_t hitGroupIndex() const override
    {
        return cloud_unbiased_ ? 8 : 6;
    }
    std::string debugName() const override
    {
        return cloud_unbiased_ ? "cloudUnbiasedVolumeAABB" : "cloudVolumeAABB";
    }
    bool unbiased() const
    {
        return cloud_unbiased_;
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
    bool cloud_unbiased_ = false;
    GfVec3f cloud_bounds_min = GfVec3f(-50.0f, 0.0f, -50.0f);
    GfVec3f cloud_bounds_max = GfVec3f(50.0f, 20.0f, 50.0f);
    float cloud_coverage = 0.35f;
    float cloud_densityScale = 0.06f;  ///< physical extinction sigma_t in 1/m
    float cloud_phaseG = 0.7f;
    float cloud_layerTop = 1.0f;
    float cloud_layerBottom = 0.0f;
    GfVec3f cloud_noiseFreq =
        GfVec3f(4.0f);  ///< per-axis (.x=horizX, .y=vert, .z=horizZ)
    GfVec3f cloud_worleyFreq = GfVec3f(4.0f);  ///< per-axis
    float cloud_detailErosion = 0.7f;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
