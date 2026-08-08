// CloudVolumeImpl implementation.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume cloud branch: reads the cloud
// shaping primvars and fills a cloud-flavoured VolumeDesc. The density itself
// is never stored — it is generated on the GPU by cloud_intersection.slang, so
// buildDensityResource is a no-op that always succeeds.
#include "cloud_volume_impl.h"

#include <spdlog/spdlog.h>

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

bool CloudVolumeImpl::parsePrimvars(
    HdSceneDelegate* sceneDelegate,
    const SdfPath& id,
    HdDirtyBits* /*dirtyBits*/)
{
    bool update_gpu_resources = false;

    // Cloud params (all optional, with sensible defaults).
    auto readFloat = [&](const char* name, float def) -> float {
        VtValue v = sceneDelegate->Get(id, TfToken(name));
        if (v.IsHolding<float>())
            return v.UncheckedGet<float>();
        if (v.IsHolding<double>())
            return float(v.UncheckedGet<double>());
        return def;
    };
    auto readVec3f = [&](const char* name, GfVec3f def) -> GfVec3f {
        VtValue v = sceneDelegate->Get(id, TfToken(name));
        if (v.IsHolding<GfVec3f>())
            return v.UncheckedGet<GfVec3f>();
        return def;
    };
    GfVec3f bm = readVec3f("boundsMin", GfVec3f(-50.0f, 0.0f, -50.0f));
    GfVec3f bM = readVec3f("boundsMax", GfVec3f(50.0f, 20.0f, 50.0f));
    if (bm != cloud_bounds_min || bM != cloud_bounds_max) {
        cloud_bounds_min = bm;
        cloud_bounds_max = bM;
        update_gpu_resources = true;
    }
    cloud_coverage = readFloat("coverage", 0.35f);
    cloud_densityScale = readFloat("densityScale", 1.2f);
    cloud_phaseG = readFloat("phaseG", 0.7f);
    cloud_layerTop = readFloat("layerTop", 1.0f);
    cloud_layerBottom = readFloat("layerBottom", 0.0f);
    cloud_noiseFreq = readFloat("noiseFreq", 4.0f);
    cloud_worleyFreq = readFloat("worleyFreq", 4.0f);
    cloud_detailErosion = readFloat("detailErosion", 0.7f);

    spdlog::info(
        "CloudVolumeImpl {} [CLOUD]: bounds [({:.1f},{:.1f},{:.1f}).."
        "({:.1f},{:.1f},{:.1f})] coverage={:.2f} density={:.2f} g={:.2f}",
        id.GetText(),
        bm[0],
        bm[1],
        bm[2],
        bM[0],
        bM[1],
        bM[2],
        cloud_coverage,
        cloud_densityScale,
        cloud_phaseG);

    return update_gpu_resources;
}

void CloudVolumeImpl::fillVolumeDesc(VolumeDesc& vd) const
{
    // bindlessIndex/gridRes/cellSize/gridMin are unused; the cloud fields drive
    // the procedural density.
    vd.bindlessIndex = 0;
    vd.gridResX = 0;
    vd.gridResY = 0;
    vd.gridResZ = 0;
    vd.cellSize = 0.0f;
    vd.gridMin =
        float3(cloud_bounds_min[0], cloud_bounds_min[1], cloud_bounds_min[2]);
    vd.volumeKind = uint32_t(VolumeKind::Cloud);
    vd.coverage = cloud_coverage;
    vd.densityScale = cloud_densityScale;
    vd.phaseG = cloud_phaseG;
    vd.layerTop = cloud_layerTop;
    vd.layerBottom = cloud_layerBottom;
    vd.noiseFreq = cloud_noiseFreq;
    vd.worleyFreq = cloud_worleyFreq;
    vd.detailErosion = cloud_detailErosion;
    vd._cloudPad = float2(0.0f, 0.0f);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
