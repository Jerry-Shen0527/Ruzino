import callable_data;
#include "utils/random.slangh"
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;
import utils.Spectrum;

// Procedural human-scale ground checker for the character walking demo.
// Same structure as checkerboard_ground.slang (Lambertian checker), but the
// 2 m cell and 40 % albedo are tuned for a camera a few meters from a 1.7 m
// character: the checker gives an unambiguous sense of speed and scale while
// staying dim enough for a tonally correct sky-lit image. A thin grid line
// on cell borders sharpens the sense of motion.
//
// The walking demo is a Z-up stage and its ground spans the X/Y plane, so
// the checker keys off pos.x + pos.y (pos.z is constantly 0 there and would
// degenerate the pattern into stripes).

[shader("callable")]
void eval_demo_ground(inout EvalCallableData data)
{
    const float cellSize = 2.0;
    float3 pos = data.vertexInfo.posW;
    float cx = floor(pos.x / cellSize);
    float cy = floor(pos.y / cellSize);
    int parity = (int(cx) + int(cy)) & 1;

    float3 color1 = float3(0.42, 0.44, 0.46);  // light squares (cool gray)
    float3 color2 = float3(0.24, 0.25, 0.27);  // dark squares
    float3 baseColor = (parity == 0) ? color1 : color2;

    // Thin bright seam where either coordinate is within ~1.5 cm of a cell
    // border — a subtle sidewalk-grid look that makes walking speed legible.
    float2 frac2 = abs(frac(pos.xy / cellSize) - 0.5);
    float edge = (0.5 - max(frac2.x, frac2.y)) * cellSize;  // meters to border
    if (edge < 0.015)
        baseColor = float3(0.55, 0.56, 0.58);

    float3 f = baseColor / 3.14159265359;
    data.color = Spectrum(f);

    data.sampledDir =
        sample_cosine_hemisphere_concentric(random_float2(data.seed), data.pdf);
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        data.vertexInfo.normalW, data.vertexInfo.tangentW, valid);
    data.sampledDir = sf.fromLocal(data.sampledDir);

    float NdotSampled = max(0.0, dot(data.vertexInfo.normalW, data.sampledDir));
    data.throughput = Spectrum(f * NdotSampled);

    float NdotL = max(0.0, dot(data.vertexInfo.normalW, data.L));
    data.bsdfPdfForLightDir = NdotL / 3.14159265359;

    data.emissionLe = Spectrum(0.0);
}
