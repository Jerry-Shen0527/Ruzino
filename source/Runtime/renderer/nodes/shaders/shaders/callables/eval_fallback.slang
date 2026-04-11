import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;

// Fallback evaluation for unsupported materials
[shader("callable")]
void eval_fallback(inout EvalCallableData data)
{
    float3 color3 = float3(0.8);  // Default color for fallback
    data.color =
        Spectrum(color3 * max(0.0, dot(data.L, data.vertexInfo.normalW)));

    data.sampledDir =
        sample_cosine_hemisphere_concentric(random_float2(data.seed), data.pdf);
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW, float4(1, 0, 0, 1), valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    data.throughput = Spectrum(float3(0.8, 0.8, 0.8) / 3.14159 *
                      max(0.0, dot(data.vertexInfo.normalW, data.sampledDir)));
}
