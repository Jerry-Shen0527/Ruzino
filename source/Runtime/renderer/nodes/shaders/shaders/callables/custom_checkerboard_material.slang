import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;

// Custom checkerboard material shader - EVAL callable
// Procedural checkerboard pattern based on UV coordinates

[shader("callable")]
void eval_custom_checkerboard_material(inout EvalCallableData data)
{
    // Create checkerboard pattern based on world position (x, y only)
    float3 pos = data.vertexInfo.posW;
    float scale = 1;
    float3 scaledPos = pos * scale;
    float checker =
        ((int(floor(scaledPos.x)) + int(floor(scaledPos.y))) % 2) == 0 ? 1.0
                                                                       : 0.0;

    // Mix between two colors
    float3 color1 = float3(1.0, 1.0, 1.0);  // White
    float3 color2 = float3(0.2);            // Dark gray
    float3 baseColor = lerp(color1, color2, checker);

    // Lambertian BRDF evaluation
    data.color = Spectrum(baseColor / 3.14159265359);

    // Sample next direction using cosine-weighted hemisphere sampling
    data.sampledDir =
        sample_cosine_hemisphere_concentric(random_float2(data.seed), data.pdf);

    // Transform from local to world space
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW, data.vertexInfo.tangentW, valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    // Lambertian BRDF for throughput
    float NdotSampled = max(0.0, dot(data.vertexInfo.normalW, data.sampledDir));
    data.throughput = Spectrum(baseColor * NdotSampled / 3.14159265359);
}
