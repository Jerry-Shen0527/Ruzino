import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;

// Fallback evaluation for unsupported materials.
// Lambertian diffuse, albedo = 0.8.
//
// data.color convention: pure BSDF value f = albedo/PI (NO NdotL here).
// The path-tracing main loop multiplies by NdotL itself when accumulating
// direct lighting, matching eval_standard_surface / eval_preview_surface.
// (The old version multiplied NdotL into data.color, which only worked
//  because the fallback branch ran its own direct-lighting loop; once the
//  fallback goes through the standard loop it would double-count NdotL.)
[shader("callable")]
void eval_fallback(inout EvalCallableData data)
{
    float3 albedo = float3(0.8);
    float3 f = albedo / 3.14159265359;  // Lambertian BRDF f = albedo / PI

    // Pure BSDF value — main loop applies NdotL
    data.color = Spectrum(f);

    // Cosine-weighted hemisphere sampling for next bounce direction
    data.sampledDir =
        sample_cosine_hemisphere_concentric(random_float2(data.seed), data.pdf);
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW, data.vertexInfo.tangentW, valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    // Throughput = f * NdotSampled (Lambertian); main loop divides by pdf.
    float NdotSampled = max(0.0, dot(data.vertexInfo.normalW, data.sampledDir));
    data.throughput = Spectrum(f * NdotSampled);

    // bsdf_pdf(L): cosine-weighted hemisphere, pdf = max(0, NdotL) / PI.
    float NdotL = max(0.0, dot(data.vertexInfo.normalW, data.L));
    data.bsdfPdfForLightDir = NdotL / 3.14159265359;
}
