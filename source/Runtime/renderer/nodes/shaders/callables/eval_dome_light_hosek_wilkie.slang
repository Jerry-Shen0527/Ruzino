import callable_data;
#include "utils/random.slangh"
import utils.Spectrum;
import Scene.Lights.HosekSkyState;
import Scene.Lights.HosekSkyEval;
import pt_sample_lights;  // for hosekStateBuffer + lightBuffer (sun lookup)

// Hosek-Wilkie analytic sky dome-light callable.
//
// Evaluates the analytic model from:
//   Hosek & Wilkie, "An Analytic Model for Full Spectral Sky-Dome
//   Radiance", ACM TOG 31(4), SIGGRAPH 2012.
//
// The per-channel polynomial state (HosekSkyState, 30 floats) is cooked on
// the CPU from (turbidity, groundAlbedo, solarElevation) and uploaded into
// hosekStateBuffer. This shader only runs the cheap per-pixel evaluation,
// which lives in Scene.Lights.HosekSkyEval (shared with the raster-side
// volumetric fog / sky-background pass).
//
// Convention (matches eval_dome_light_procedural_sky.slang /
// eval_dome_light_default.slang): +Y is up in dome-local space. theta is the
// view zenith angle (0 at zenith), gamma is the angle between view direction
// and sun direction. The sun direction is found from the scene's first
// DistantLight so the sky and the sun light stay aligned (the sky returns
// DIFFUSE sky radiance only; the solar disc itself is provided by the
// DistantLight, avoiding double counting).

[shader("callable")]
void eval_dome_light_hosek_wilkie(inout DomeLightCallableData data)
{
    HosekSkyState sky = hosekStateBuffer[data.hosekStateIndex];

    float3 viewDir = normalize(data.localDirection);
    data.envColor = Spectrum(evalHosekSkyRadiance(
        sky, viewDir, data.sunDirLocal, data.intensity));
}
