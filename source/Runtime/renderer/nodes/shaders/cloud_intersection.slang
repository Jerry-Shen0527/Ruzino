// Cloud rendering helpers for the procedural path-traced volume.
//
// Like volume_intersection.slang, this is a pure IMPORTABLE MODULE: no
// [shader] entry points, no references to RayPayload/ShadowPayload. The cloud
// hit-group entry points (CloudIntersection / CloudClosestHit /
// CloudShadowHit) live inside path_tracing.slang (same translation unit) so
// they can see the file-level RayPayload and the global buffers.
//
// Cloud density model (Horizon: Zero Dawn / Bille / Hillaire family):
//   - base shape  = Perlin fbm (low-frequency) shaped by an inverted Worley
//                   "billow" and a height gradient (cumulus: dense bottom,
//                   wispy top) and a coverage threshold;
//   - detail      = higher-frequency Worley F1/F2, subtracted to erode edges;
//   - density     = clamp(shape - erosion, 0, 1) -> [0,1] majorant for the
//                   Beer-Lambert/HG integration done in the hit shaders.
//
// All noise is sampled in NORMALIZED [0,1]^3 space over the volume AABB, so
// cloud scale is independent of world size (a 1km and a 10m cloud read the
// same noise as long as the AABB is set).
import Scene.SceneTypes;
import Scene.BindlessVertexBuffer;

// Include the MaterialX stdlib noise library (OSL-derived, pure slang). It
// lives under the MaterialX libraries search root, added to the slang
// session in render_global_payload.cpp::resolve_mtlx_library_path().
#include "stdlib/genslang/lib/mx_noise.slang"

// Henyey-Greenstein phase function (scalar asymmetry g). Inlined here rather
// than imported from Rendering/Volumes/PhaseFunction.slang because that module
// pulls in the IPhaseFunction interface + anyValueSize import chain, which
// produces DXIL that D3D12 rejects at PSO creation. The math is identical.
float evalHenyeyGreenstein(float cosTheta, float g)
{
    const float denom = 1.0 + g * g + 2.0 * g * cosTheta;
    return (1.0 / (4.0 * 3.14159265358979)) * (1.0 - g * g)
         / (denom * sqrt(max(denom, 1e-8)));
}

// Cloud hit attributes — same shape as VolumeHitAttributes (slab entry/exit),
// kept separate for clarity (a cloud always reports the full segment, whereas
// the wetbrush slab reports only where density crosses a threshold).
struct CloudHitAttributes {
    float tEnter;  // Ray param at cloud AABB entry.
    float tExit;   // Ray param at cloud AABB exit (clamped to RayTCurrent).
};

// Slab AABB ray test in world space (identical math to volume_intersection's
// intersectSlab, duplicated here to keep the cloud module self-contained).
bool cloudIntersectSlab(VolumeDesc vd, out float tEnter, out float tExit)
{
    float3 invD = 1.0 / WorldRayDirection();
    float3 t0 = (vd.boundsMin - WorldRayOrigin()) * invD;
    float3 t1 = (vd.boundsMax - WorldRayOrigin()) * invD;
    float3 tsmaller = min(t0, t1);
    float3 tbigger = max(t0, t1);
    tEnter = max(tsmaller.x, max(tsmaller.y, tsmaller.z));
    tExit = min(tbigger.x, min(tbigger.y, tbigger.z));
    tEnter = max(tEnter, RayTMin());
    tExit = min(tExit, RayTCurrent());
    return tEnter < tExit;
}

// Map a world-space position to normalized [0,1]^3 over the cloud AABB.
float3 cloudNormalizedPos(VolumeDesc vd, float3 p)
{
    return (p - vd.boundsMin) / max(vd.boundsMax - vd.boundsMin, float3(1e-6));
}

// Height gradient: dense at the cloud base (layerBottom), wispy at the top
// (layerTop). A smooth ramp between the two; outside the layer -> 0.
float cloudHeightGradient(VolumeDesc vd, float h01)
{
    // h01 is the normalized height fraction within the AABB (y / size).
    float lo = vd.layerBottom;
    float hi = vd.layerTop;
    if (h01 < lo || h01 > hi)
        return 0.0;
    float t = (h01 - lo) / max(hi - lo, 1e-4);
    // Lower-falloff (dense bottom), upper-falloff (wispy top): both smoothstep.
    float lower = 1.0 - smoothstep(0.0, 0.2, t);   // dense at t<0.2
    float upper = 1.0 - smoothstep(0.6, 1.0, t);   // thin out near top
    return max(lower, upper * 0.5 + 0.5);          // combine
}

// Procedural cloud density at a world-space position, in [0,1].
// Returns 0 outside the cloud layer / where coverage is not met.
float cloudDensity(VolumeDesc vd, float3 p)
{
    float3 q = cloudNormalizedPos(vd, p) * vd.noiseFreq;
    float3 qw = cloudNormalizedPos(vd, p) * vd.worleyFreq;

    // Base shape: Perlin fbm in [-1,1] -> [0,1].
    float base = mx_fractal_noise_float(q, 5, 2.0, 0.5);   // ~[-1,1]
    base = base * 0.5 + 0.5;                               // ->[0,1]

    // Inverted Worley "billow": dilates the fbm so clouds look puffy rather
    // than smoothly gradient. F1 in ~[0,1] cell units; invert so the dense
    // interior reads high.
    float worleyF1 = mx_worley_noise_float(qw, 1.0, 0);  // Euclidean F1
    float billow = 1.0 - clamp(worleyF1, 0.0, 1.0);

    float hg = cloudHeightGradient(vd, cloudNormalizedPos(vd, p).y);

    // Coverage: subtract a threshold so coverage controls how much sky is
    // cloud. Combine base*billow with the height gradient.
    float shape = base * billow * hg;
    shape -= vd.coverage;

    // Detail erosion: Worley F1/F2 high-frequency, subtracted to carve wispy
    // edges (the signature "cloud" silhouette).
    float2 detail = mx_worley_noise_vec2(qw * 3.0, 1.0, 0);  // (F1, F2)
    float erode = clamp(detail.x * vd.detailErosion, 0.0, 1.0);
    shape -= erode * 0.5;

    return clamp(shape, 0.0, 1.0);
}

// Transmittance toward the sun along direction L from point p, integrated
// through the SAME procedural density field (the "light march"). Returns the
// Beer-Lambert transmittance [0,1] and accumulates a powder term.
//
// This is the HZD/GPU-Pro cheap NEE: 6 short steps toward the sun + one long
// step for shadows cast by distant clouds. Each step: T *= exp(-sigma_t*d*dt),
// plus a powder factor (1 - exp(-2*sigma_t*d*dt)) that darkens dense regions
// facing away from the sun (the dark cloud base).
float cloudLightMarch(VolumeDesc vd, float3 p, float3 L, float sunDist,
                      out float powder)
{
    const int STEPS = 6;
    float stepLen = sunDist / float(STEPS);
    float T = 1.0;
    powder = 0.0;
    float3 stepP = p + L * (stepLen * 0.5);
    for (int i = 0; i < STEPS; ++i) {
        float d = cloudDensity(vd, stepP);
        float ext = d * vd.densityScale * stepLen;
        T *= exp(-ext);
        // Accumulate powder (max over the march gives the strongest darkening).
        powder = max(powder, 1.0 - exp(-2.0 * ext));
        stepP += L * stepLen;
        if (T < 0.01)
            break;
    }
    return T;
}
