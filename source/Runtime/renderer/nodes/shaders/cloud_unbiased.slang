// Unbiased volumetric cloud helpers (delta tracking / ratio tracking).
//
// This is the UNBIASED counterpart of cloud_intersection.slang's quadrature
// march, kept in a separate module so the biased pipeline stays untouched:
//
//   biased pipeline (volumeType="cloud", hit-group slots 6/7):
//       fixed-step Beer-Lambert quadrature + heuristic multi-scatter/powder
//       terms. Biased (discretization) but low-variance per sample.
//
//   unbiased pipeline (volumeType="cloud_unbiased", hit-group slots 8/9):
//       delta tracking for free-flight sampling + ratio tracking for
//       transmittance + phase-sampled path continuation. Unbiased by
//       construction; variance is bought down by frame accumulation instead
//       of quadrature bias.
//
// Why this is unbiased:
//   - The density field is normalized to [0,1], so sigma_t <= densityScale:
//     the majorant sigma_bar = vd.densityScale is EXACT (no majorant grid
//     needed). Null-collision sampling therefore thins a Poisson(sigma_bar)
//     process with keep-probability d(t) into an inhomogeneous Poisson with
//     rate sigma_t(t) — the accepted event position is distributed with pdf
//     sigma_t(t) * T(t) (Woodcock 1965, Miller et al. 2019), which is the
//     exact free-flight distribution. The in-scatter estimator at the event
//     carries weight sigma_s/sigma_t = 1 (clouds scatter conservatively),
//     so no transmittance factor appears at all — the pdf cancelled it.
//   - The escape transmittance is the ratio-tracking product over the same
//     candidate points: E[Π (1 - d(t_i))] = exp(-∫ sigma_t) (Novak et al.
//     2014, unbiased for Poisson-distributed samples).
//   - The sun NEE uses an independent ratio-tracking transmittance.
//   - Multiple scattering emerges from continuing the path along a
//     Henyey-Greenstein-sampled direction after each real event — no
//     heuristic white-interior term. Environment light is picked up when the
//     scattered path escapes the volume and misses (the miss shader).
//
// RNG: seed is passed BY VALUE and mutated locally (xorshift32). An inout
// seed parameter crossing the module import boundary (or an SV_RayPayload
// member used as inout) makes slang ICE, so no state escapes here — each
// call draws its own independent stream, which is enough for unbiasedness.
import Scene.SceneTypes;    // VolumeDesc
import cloud_intersection;  // cloudDensity / cloudExitDist / evalHenyeyGreenstein

uint cloudUnbiasedXorshift(uint seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

float cloudUnbiasedRandom(inout uint seed)
{
    seed = cloudUnbiasedXorshift(seed);
    return float(seed) * (1.0 / 4294967296.0);
}

// Unbiased Beer-Lambert transmittance from p along direction L (normalized)
// up to maxDist, by ratio tracking: sample Poisson(densityScale) candidate
// points and multiply (1 - d) at each. No step size, no early-out — the
// estimate is unbiased regardless of how the density varies between samples.
float cloudRatioTrack(VolumeDesc vd, float3 p, float3 L, float maxDist,
                      uint seed)
{
    float sigmaBar = vd.densityScale;
    float T = 1.0;
    float t = -log(max(1.0 - cloudUnbiasedRandom(seed), 1e-9)) / sigmaBar;
    while (t < maxDist) {
        float d = cloudDensity(vd, p + L * t);
        T *= (1.0 - d);
        t += -log(max(1.0 - cloudUnbiasedRandom(seed), 1e-9)) / sigmaBar;
    }
    return T;
}

// Sample a direction with pdf Henyey-Greenstein(cosTheta, g) around `dir`.
// Closed-form inverse CDF; the returned pdf exactly matches the HG phase, so
// a phase-sampled path continuation carries weight 1 (albedo 1 medium).
float3 cloudSampleHG(float3 dir, float g, float u1, float u2)
{
    float cost;
    if (abs(g) < 1e-4) {
        cost = 1.0 - 2.0 * u1;
    } else {
        float a = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
        cost = (1.0 + g * g - a * a) / (2.0 * g);
    }
    float sint = sqrt(max(1.0 - cost * cost, 0.0));
    float phi = 2.0 * 3.14159265358979 * u2;
    float sinp = sin(phi), cosp = cos(phi);

    // Orthonormal basis around dir.
    float3 up = abs(dir.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, dir));
    float3 bitangent = cross(dir, tangent);
    return normalize(dir * cost + tangent * (sint * cosp)
                     + bitangent * (sint * sinp));
}
