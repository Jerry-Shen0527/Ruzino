import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;

// Procedural checkerboard Lambertian material for GROUND planes.
//
// The generic custom_checkerboard_material.slang keys its pattern off
// posW.x + posW.y, which is correct for a vertical wall spanning X/Y but
// degenerates to stripes on a horizontal ground plane (where y is constant).
// This shader uses posW.x + posW.z so the checker lies in the ground's XZ
// plane. `cellSize` controls the side length of one square in world units;
// 8.0 reads well against the cloud scene's ±5000 ground and the camera at
// height 8. The two albedos are darkened (0.5 / 0.25) so the ground reads as
// a tonally-correct surface rather than the blown-out fallback 0.8 white.

[shader("callable")]
void eval_checkerboard_ground(inout EvalCallableData data)
{
    // Cell size in world meters. Scaled to match the physically-sized cloud
    // scene (~1 km altitude, camera ~1 km away) so the checker is still legible
    // at distance — an 8 m cell aliases to grey from 1 km. 200 m ≈ a field
    // block, readable across the whole frame.
    const float cellSize = 200.0;
    float3 pos = data.vertexInfo.posW;
    float cx = floor(pos.x / cellSize);
    float cz = floor(pos.z / cellSize);
    // int parity so negative coordinates still alternate correctly.
    int parity = (int(cx) + int(cz)) & 1;
    float3 color1 = float3(0.5);   // light squares
    float3 color2 = float3(0.25);  // dark squares
    float3 baseColor = (parity == 0) ? color1 : color2;

    // Lambertian BRDF f = albedo / PI (pure BSDF, no NdotL — caller applies it).
    float3 f = baseColor / 3.14159265359;
    data.color = Spectrum(f);

    // Cosine-weighted hemisphere sampling for the next bounce direction.
    data.sampledDir =
        sample_cosine_hemisphere_concentric(random_float2(data.seed), data.pdf);
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW, data.vertexInfo.tangentW, valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    // Throughput = f * NdotSampled; caller divides by pdf -> effective weight
    // = albedo (the cosine-sampler pdf = NdotSampled/PI cancels).
    float NdotSampled = max(0.0, dot(data.vertexInfo.normalW, data.sampledDir));
    data.throughput = Spectrum(f * NdotSampled);

    // bsdf_pdf for the NEE light direction (cosine hemisphere: NdotL/PI),
    // used by MIS on the direct-lighting path.
    float NdotL = max(0.0, dot(data.vertexInfo.normalW, data.L));
    data.bsdfPdfForLightDir = NdotL / 3.14159265359;

    data.emissionLe = Spectrum(0.0);
}
