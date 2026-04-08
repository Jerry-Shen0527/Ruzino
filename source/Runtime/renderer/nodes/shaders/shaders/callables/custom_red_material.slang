import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import Utils.Math.MathHelpers;
import utils.Spectrum;

// Custom red material shader - EVAL callable
// This is a simple diffuse red material

[shader("callable")]
void eval_custom_red_material(inout EvalCallableData data)
{
    // Simple red diffuse material
    float3 baseColor = float3(1.0, 0.0, 0.0);

    // Lambertian BRDF evaluation
    float NdotL = max(0.0, dot(data.vertexInfo.normalW, data.L));
    data.color = Spectrum(baseColor * NdotL / 3.14159265359);

    // Sample next direction using cosine-weighted hemisphere sampling
    data.sampledDir = sample_cosine_hemisphere_concentric(
        random_float2(data.seed), data.pdf);

    // Transform from local to world space
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW,
        data.vertexInfo.tangentW,
        valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    // Lambertian BRDF for throughput: albedo / pi * cos(theta)
    float NdotSampled = max(0.0, dot(data.vertexInfo.normalW, data.sampledDir));
    data.throughput = Spectrum(baseColor * NdotSampled / 3.14159265359);
}
