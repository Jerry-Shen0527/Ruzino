// Cloud rendering helpers for the procedural path-traced volume.
//
// Like volume_intersection.slang, this is a pure IMPORTABLE MODULE: no
// [shader] entry points, no references to RayPayload/ShadowPayload. The cloud
// hit-group entry points (CloudIntersection / CloudClosestHit /
// CloudShadowHit) live inside path_tracing.slang (same translation unit) so
// they can see the file-level RayPayload and the global buffers.
//
// Cloud density model (Hillaire 2020 / Nubis family):
//   - base      = low-frequency fbm — the large soft billows; the layer spans
//                 ~noiseFreq cells per axis, so one clump is ~1/noiseFreq of
//                 the layer width (a real cumulus-sized puff);
//   - billow    = inverted Worley F1 at cell scale, MULTIPLIED with the fbm
//                 so the field is low-mean and clumpy (distinct puffs with
//                 gaps, not one connected mass);
//   - profile   = vertical cumulus cross-section (flat base, wispy top),
//                 0 -> 1 -> 0 across the layer;
//   - coverage  = threshold on that shape remapped with a SOFT smoothstep
//                 shoulder so puffs fade in/out instead of switching on;
//   - detail    = higher-frequency Worley F1, subtracted proportionally to
//                 carve the cauliflower lobes and separate puffs.
//
// Extinction is PHYSICAL: sigma_t = density * densityScale, with densityScale
// interpreted as the extinction coefficient in 1/m (typical cumulus ~0.05/m,
// i.e. a mean free path of ~20 m). The old arbitrary multiplier (~2.5/m,
// MFP 0.4 m) made every ray-march step saturate exp(-sigma_t*step) to 0/1,
// which quantized the transmittance into binary light/dark bands (the NPR
// look) — with the physically-scaled coefficient and jittered marching the
// Beer-Lambert integral stays smooth.
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

// Vertical cumulus cross-section: dense just above the base (the flat cloud
// bottom), thinning to a wispy top (the anvil). Returns 0 outside the
// [layerBottom, layerTop] band and 0 -> 1 -> 0 across it.
float cloudHeightGradient(VolumeDesc vd, float h01)
{
    // h01 is the normalized height fraction within the AABB (y / size).
    float lo = vd.layerBottom;
    float hi = vd.layerTop;
    float t = (h01 - lo) / max(hi - lo, 1e-4);
    if (t <= 0.0 || t >= 1.0)
        return 0.0;
    // Sharp lower ramp = flat base; long upper falloff = wispy top.
    float bottom = smoothstep(0.0, 0.15, t);
    float top = 1.0 - smoothstep(0.55, 1.0, t);
    return bottom * top;
}

// Procedural cloud density at a world-space position, in [0,1].
// Returns 0 outside the cloud layer / where coverage is not met.
float cloudDensity(VolumeDesc vd, float3 p)
{
    float3 np = cloudNormalizedPos(vd, p);

    // Base macro shape: low-frequency fbm. ~noiseFreq cells span the layer,
    // so each clump is ~1/noiseFreq of the layer width — the large soft
    // billows the cloud field is built from.
    float3 q = np * vd.noiseFreq;
    float base = mx_fractal_noise_float(q, 4, 2.0, 0.5);   // ~[-1,1]
    base = base * 0.5 + 0.5;                               // -> [0,1]

    // Inverted-Worley "billow" at cell scale: round cellular lobes. The
    // PRODUCT with the fbm keeps the field low-mean and clumpy — distinct
    // puffs with gaps between them. (The old max() combine biased the field
    // high, so the coverage threshold left a uniform connected sheet with
    // barely any structure.)
    float3 qw = np * vd.worleyFreq;
    float billow = 1.0 - saturate(mx_worley_noise_float(qw, 1.0, 0));

    // Vertical cumulus profile (flat base, wispy top).
    float profile = cloudHeightGradient(vd, np.y);

    // Coverage: threshold with a SOFT shoulder. smoothstep turns the cutoff
    // into a gradual 0->1 ramp so puffs fade in/out instead of switching on
    // at full density — the old hard `shape -= coverage` clip made the field
    // binary, which produced the uniform "white wall" and the stepwise
    // light/dark shading bands. Lower coverage = more cloud.
    float shape = base * billow * profile;
    shape = smoothstep(vd.coverage, vd.coverage + 0.35, shape);

    // Detail erosion: a higher-frequency Worley F1 carves the cauliflower
    // lobes. F1 is the distance to the nearest cell feature, so it peaks at
    // cell boundaries — eroding there separates puffs and ruffles the
    // silhouette. Proportional erosion keeps the field in [0,1] and eats the
    // soft rim rather than the dense core.
    float2 detail = mx_worley_noise_vec2(qw * 3.0, 1.0, 0);  // (F1, F2)
    float erode = saturate(detail.x * 1.4) * vd.detailErosion;
    shape -= erode * shape;
    return saturate(shape);
}

// Distance from p (inside the AABB) to the AABB exit along the normalized
// direction L — bounds the light march instead of the old fixed half-width
// guess, which under/overshot for anisotropic layers.
float cloudExitDist(VolumeDesc vd, float3 p, float3 L)
{
    // Guard exact-zero components so (bound - p) / L cannot produce 0/0 NaN.
    // (Written with plain ifs: the equivalent select() expression makes slang
    // ICE with "unknown stage" when the whole module is compiled as a
    // multi-stage library.)
    float3 safeL = L;
    if (abs(safeL.x) < 1e-9)
        safeL.x = 1e-9;
    if (abs(safeL.y) < 1e-9)
        safeL.y = 1e-9;
    if (abs(safeL.z) < 1e-9)
        safeL.z = 1e-9;
    float3 t0 = (vd.boundsMin - p) / safeL;
    float3 t1 = (vd.boundsMax - p) / safeL;
    return max(min(max(t0.x, t1.x), min(max(t0.y, t1.y), max(t0.z, t1.z))), 0.0);
}

// Transmittance toward the sun along direction L from point p, integrated
// through the SAME procedural density field (the "light march"). Returns the
// Beer-Lambert transmittance [0,1] and the beer-powder term.
//
// The march has two regimes, because it must be cheap but resolve two very
// different scales:
//   - NEAR field (first ~320 m, or the whole distance for small clouds):
//     fine steps ~MFP-scale so the LOCAL extinction — and therefore the
//     shading gradient on the cloud surface — is smooth. With the physical
//     sigma_t (~0.05-0.1/m) the mean free path is ~10-20 m, so ~40 m steps
//     resolve it. This is what removes the old NPR banding.
//   - FAR field: a few wide samples catch distant puffs that shadow this
//     point. A puff is opaque, so one hit saturates the optical depth and the
//     march stops — exactly the hard shadow a real cumulus casts.
//
// The whole pattern is JITTERED per ray via jitter0/jitter1 (computed by the
// caller from its random seed — kept out of this module's signature because
// slang ICEs on inout seed parameters crossing an import boundary): residual
// stair-step noise decorrelates across samples and the frame accumulation
// averages it out. Beer-powder is accumulated as powder = 1 - T^2 (the
// fraction of light absorbed on the way in AND out again) — high deep inside
// dense cloud (dark base), low on sunlit rims.
float cloudLightMarch(VolumeDesc vd, float3 p, float3 L, float maxDist,
                      float jitter0, float jitter1, out float powder)
{
    const float NEAR_STEP = 40.0;  // ~2-3 mean free paths at sigma_t=0.06
    const int NEAR_N = 8;          // ~320 m of fine resolution
    const int FAR_N = 3;           // distant shadow samples
    float sigmaT = vd.densityScale;

    float od = 0.0;  // accumulated optical depth along L

    // Near field: adapt the step so small clouds (maxDist < NEAR_N*NEAR_STEP)
    // still get the full sample count instead of one giant step.
    float nearDist = min(maxDist, float(NEAR_N) * NEAR_STEP);
    float nearStep = nearDist / float(NEAR_N);
    float t = nearStep * jitter0;
    for (int i = 0; i < NEAR_N; ++i) {
        if (t >= nearDist)
            break;
        float d = cloudDensity(vd, p + L * t);
        od += d * sigmaT * nearStep;
        if (od > 6.0)   // T < 0.0025: nothing visible left to lose
            break;
        t += nearStep;
    }

    // Far field: wide samples for shadows cast by distant puffs.
    float farDist = maxDist - nearDist;
    if (od <= 6.0 && farDist > 0.0) {
        float farStep = farDist / float(FAR_N);
        t = nearDist + farStep * jitter1;
        for (int i = 0; i < FAR_N; ++i) {
            float d = cloudDensity(vd, p + L * t);
            od += d * sigmaT * farStep;
            if (od > 6.0)
                break;
            t += farStep;
        }
    }

    float T = exp(-od);
    powder = 1.0 - T * T;
    return T;
}
