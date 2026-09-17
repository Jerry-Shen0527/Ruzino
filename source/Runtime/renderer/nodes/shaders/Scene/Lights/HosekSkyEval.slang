// HosekSkyEval.slang
//
// Importable Hosek-Wilkie analytic-sky evaluation, shared by the RT dome
// callable (eval_dome_light_hosek_wilkie.slang) and the real-time raster-side
// consumers (volumetric_fog_composite.cs.slang background/ambient term).
// Pure functions only — no [shader] entry points, no resource declarations —
// so it can be imported from any stage without pulling in the path-tracer
// binding surface.
//
// Convention (matches the callable): directions are DOME-LOCAL with +Y up.
// theta is the view zenith angle (0 at zenith), gamma the view-sun angle.
// The sky returns DIFFUSE sky radiance only; the solar disc is provided by a
// separate DistantLight, avoiding double counting.
import Scene.Lights.HosekSkyState;

// The model's per-channel radiance core, ported algebra-for-algebra from
// ArHosekSkyModel_GetRadianceInternal (v1.4a):
//   L(theta,gamma) = (1 + c0*exp(c1/cos(theta+0.01)))
//                  * (c2 + c3*exp(c4*gamma) + c5*cos^2(gamma)
//                     + c6*mieM + c7*sqrt(cos(theta)))
// where mieM = (1+cos^2 gamma) / (1 + c8^2 - 2*c8*cos gamma)^1.5.
float hosekRadianceInternal(const float config[9], float theta, float gamma)
{
    const float cosTheta = cos(theta);
    const float cosGamma = cos(gamma);

    const float expM = exp(config[4] * gamma);
    const float rayM = cosGamma * cosGamma;
    const float mieM = (1.0 + rayM)
        / pow(1.0 + config[8] * config[8] - 2.0 * config[8] * cosGamma, 1.5);
    const float zenith = sqrt(cosTheta);

    return (1.0 + config[0] * exp(config[1] / (cosTheta + 0.01)))
         * (config[2] + config[3] * expM + config[5] * rayM
            + config[6] * mieM + config[7] * zenith);
}

// Normalize the dome-local sun direction, falling back to a ~45 deg elevation
// when it is missing. Clamped to above-horizon: the Hosek dataset is only
// defined for sun elevation >= 0, and a below-horizon sun would produce NaNs.
float3 hosekResolveSunDir(float3 sunDirLocal)
{
    float3 d = normalize(sunDirLocal);
    if (any(isnan(d)) || all(d == float3(0.0)))
        return normalize(float3(0.0, 0.7, 0.0));  // ~45 deg elevation, south
    d.y = max(d.y, 0.0);
    return normalize(d);
}

// Evaluate the cooked Hosek-Wilkie sky radiance for one dome-local direction.
// Below-horizon directions return black (Hosek is sky-only); `intensity` is
// the DomeLight's per-channel intensity.
float3 evalHosekSkyRadiance(
    HosekSkyState sky,
    float3 viewDirLocal,  // normalized, dome-local (+Y up)
    float3 sunDirLocal,   // dome-local sun direction (see hosekResolveSunDir)
    float3 intensity)
{
    if (viewDirLocal.y < 0.0)
        return float3(0.0);

    float3 sunDir = hosekResolveSunDir(sunDirLocal);

    // theta clamped just below the horizon — Hosek's 1/(cos+0.01) term would
    // otherwise diverge exactly at the horizon (Blender's clamp).
    const float PI_2 = 1.5707963267948966;
    float theta = acos(clamp(viewDirLocal.y, -1.0, 1.0));
    theta = min(theta, PI_2 - 0.001);
    float cosGamma = clamp(dot(viewDirLocal, sunDir), -1.0, 1.0);
    float gamma = acos(cosGamma);

    float r = hosekRadianceInternal(sky.configs[0], theta, gamma) * sky.radiances[0];
    float g = hosekRadianceInternal(sky.configs[1], theta, gamma) * sky.radiances[1];
    float b = hosekRadianceInternal(sky.configs[2], theta, gamma) * sky.radiances[2];

    // The RGB dataset yields sRGB-primary radiance; the 2*pi/683 factor is the
    // HDR display scale Blender applies.
    const float SCALE = 6.283185307179586 / 683.0;
    return float3(r, g, b) * SCALE * intensity;
}
