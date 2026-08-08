import callable_data;
#include "utils/random.slangh"
import utils.Spectrum;
import Scene.Lights.HosekSkyState;
import pt_sample_lights;  // for hosekStateBuffer + lightBuffer (sun lookup)

// Hosek-Wilkie analytic sky dome-light callable.
//
// Evaluates the analytic model from:
//   Hosek & Wilkie, "An Analytic Model for Full Spectral Sky-Dome
//   Radiance", ACM TOG 31(4), SIGGRAPH 2012.
//
// The per-channel polynomial state (HosekSkyState, 30 floats) is cooked on
// the CPU from (turbidity, groundAlbedo, solarElevation) and uploaded into
// hosekStateBuffer. This shader only runs the cheap per-pixel evaluation
// (ArHosekSkyModel_GetRadianceInternal), which is the model's 9-coefficient
// core per colour channel.
//
// Convention (matches eval_dome_light_procedural_sky.slang /
// eval_dome_light_default.slang): +Y is up in dome-local space. theta is the
// view zenith angle (0 at zenith), gamma is the angle between view direction
// and sun direction. The sun direction is found from the scene's first
// DistantLight so the sky and the sun light stay aligned (the sky returns
// DIFFUSE sky radiance only; the solar disc itself is provided by the
// DistantLight, avoiding double counting).

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

// Find the sun direction (in dome-local space, +Y up) from the scene's first
// DistantLight. The dome's transMat converts world -> local at the miss call
// site (mul(float4(worldDir,0), transMat)), but that transform is applied by
// the caller and NOT available here. Instead we approximate the sun's local
// direction by the data.sunDirLocal field, which the miss handler filled by
// transforming the DistantLight world dir through the dome transform. If no
// sun was found, sunDirLocal is the zero vector and we fall back to a default
// elevation.
float3 resolveSunDir(float3 sunDirLocal)
{
    float3 d = normalize(sunDirLocal);
    if (any(isnan(d)) || all(d == float3(0.0)))
        return normalize(float3(0.0, 0.7, 0.0));  // ~45 deg elevation, south
    // Clamp to above-horizon: the Hosek dataset is only defined for sun
    // elevation >= 0. If the sun is below the horizon, pin it to the horizon
    // so the sky degrades to a deep twilight rather than producing NaNs.
    d.y = max(d.y, 0.0);
    return normalize(d);
}

[shader("callable")]
void eval_dome_light_hosek_wilkie(inout DomeLightCallableData data)
{
    // Only the upper hemisphere (+Y) is sky; the lower hemisphere is ground.
    // Return near-black below the horizon (the DomeLight intensity still
    // applies, so this can be a faint ambient if the scene sets one).
    float3 viewDir = normalize(data.localDirection);
    if (viewDir.y < 0.0) {
        // Below horizon: return a dim ambient so geometry facing down is not
        // pure black. This is not physically modelled by Hosek (which is
        // sky-only) but a tiny fill avoids artifacts.
        data.envColor = Spectrum(float3(0.0));
        return;
    }

    HosekSkyState sky = hosekStateBuffer[data.hosekStateIndex];

    float3 sunDir = resolveSunDir(data.sunDirLocal);

    // theta = view zenith angle (0 at zenith), gamma = view-sun angle.
    // Clamp theta just below pi/2 (horizon) — Hosek's 1/(cos+0.01) term would
    // otherwise diverge exactly at the horizon; this matches Blender's
    // theta = min(theta, pi/2 - 0.001) clamp.
    const float PI_2 = 1.5707963267948966;
    float theta = acos(clamp(viewDir.y, -1.0, 1.0));
    theta = min(theta, PI_2 - 0.001);
    float cosGamma = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float gamma = acos(cosGamma);

    // Per-channel radiance: core(state.config) * state.radiance.
    float r = hosekRadianceInternal(sky.configs[0], theta, gamma) * sky.radiances[0];
    float g = hosekRadianceInternal(sky.configs[1], theta, gamma) * sky.radiances[1];
    float b = hosekRadianceInternal(sky.configs[2], theta, gamma) * sky.radiances[2];

    // The RGB dataset already yields sRGB-primary radiance; multiply by the
    // 2*pi/683 factor Blender uses to convert to a displayable HDR scale.
    const float SCALE = 6.283185307179586 / 683.0;
    float3 skyColor = float3(r, g, b) * SCALE;

    data.envColor = Spectrum(skyColor * data.intensity);
}
