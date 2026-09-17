// volumetric_fog_composite.cs.slang
//
// Real-time volumetric fog (V1: analytic exponential height fog) + sky
// background for the rasterization pipeline.
//
//   rasterize (G-Buffer) ──► deferred_direct_lighting ──► volumetric_fog_composite ──► present_color
//
// Per pixel, rebuilds the camera ray from the view constants (raygen.slang
// idiom, raster NDC orientation), marches 32 fixed steps through an
// exponential height-fog density field rho(h) = density * exp(-(h-ground)/H),
// and composites single scattering over the deferred-lit surface color:
//
//   result = sceneRadiance * T  +  integral(sigma_s * L_i(p) * T) ds
//
// In-scatter lighting (unshadowed — the deferred path has no TLAS):
//   - analytic lights from lightBuffer (Distant/Directional/Point), with a
//     Henyey-Greenstein phase;
//   - a Hosek-Wilkie sky ambient: the sky-radiance field is approximated by
//     one evaluation in a sun-ward-blend direction (the phase then gives the
//     directional variation);
//   - background pixels (no G-buffer geometry) are filled with the Hosek sky
//     radiance attenuated by the fog transmittance — this simultaneously
//     gives the raster path a real-time sky background.
//
// Slot numbers MUST match the BindingSetDesc in node_volumetric_fog_composite.cpp
// (t10/t11 mirror the deferred node's G-Buffer SRV convention; lightBuffer
// stays at t6; hosekStateBuffer takes t7 — free now that no material modules
// are imported).
#include "utils/view_cb.h"
import Scene.Lights.LightData;
import Scene.Lights.HosekSkyState;
import Scene.Lights.HosekSkyEval;

// ---[ Inputs ]---
StructuredBuffer<LightData> lightBuffer : register(t6);
StructuredBuffer<HosekSkyState> hosekStateBuffer : register(t7);

Texture2D<float4> gSceneColor : register(t10);  // deferred DirectLight Color
Texture2D<float4> gWorldPos : register(t11);     // rasterize Position (alpha = coverage)

// ---[ Constants ]---
ConstantBuffer<PlanarViewConstants> viewConstant : register(b0);

cbuffer FogConstants : register(b1)
{
    float density;        // extinction scale rho0 at the ground plane (1/m)
    float heightFalloff;  // H: height over which density falls by e (m)
    float groundHeight;   // world Y of the fog base plane (m)
    float phaseG;         // Henyey-Greenstein asymmetry [-1, 1]
    float3 scatterAlbedo; // sigma_s / sigma_t (per channel)
    float maxDistance;    // march length for background rays (m)
    uint lightCount;
    uint skyBackground;   // 1 = fill background pixels with the Hosek sky
    float _pad0;
    float _pad1;
}

// ---[ Output ]---
RWTexture2D<float4> output : register(u0);

static const float PI = 3.14159265359;

// Henyey-Greenstein phase (inlined, not imported from
// Rendering/Volumes/PhaseFunction.slang — that module's interface chain
// produces DXIL D3D12 rejects at PSO creation; see cloud_intersection.slang).
float evalHenyeyGreensteinFog(float cosTheta, float g)
{
    const float denom = 1.0 + g * g + 2.0 * g * cosTheta;
    return (1.0 / (4.0 * PI)) * (1.0 - g * g)
         / (denom * sqrt(max(denom, 1e-8)));
}

// Fog extinction coefficient at a world position: exponential height falloff,
// saturated at rho0 below the base plane.
float fogSigmaT(float3 p)
{
    float h = max(p.y - groundHeight, 0.0);
    return density * exp(-h / max(heightFalloff, 1e-4));
}

// Unshadowed in-scatter radiance at p: analytic lights + sky ambient.
// vTowardCamera points from p back to the eye.
float3 fogInscatterRadiance(float3 p, float3 vTowardCamera, float3 skyAmbient)
{
    float3 sum = float3(0.0);

    for (uint i = 0; i < lightCount; ++i) {
        LightData light = lightBuffer[i];

        float3 L;  // direction from p toward the light
        float radianceScale;
        if (light.type == uint(LightType::Distant) ||
            light.type == uint(LightType::Directional)) {
            L = normalize(-light.dirW);
            radianceScale = 1.0;
        }
        else if (light.type == uint(LightType::Point)) {
            float3 toLight = light.posW - p;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            radianceScale = 1.0 / max(dist * dist, 0.001);
        }
        else {
            continue;  // area lights / domes: domes are the sky ambient below
        }

        float phase = evalHenyeyGreensteinFog(dot(L, vTowardCamera), phaseG);
        sum += light.intensity * radianceScale * phase;
    }

    // Sky ambient: L_sky already carries the phase-folded hemisphere
    // integral approximation (see caller); just add its magnitude.
    sum += skyAmbient;
    return sum;
}

[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 dim;
    gSceneColor.GetDimensions(dim.x, dim.y);
    if (dispatchThreadID.x >= dim.x || dispatchThreadID.y >= dim.y)
        return;

    float4 sceneColor = gSceneColor[dispatchThreadID];
    float4 worldPos = gWorldPos[dispatchThreadID];
    bool isBackground = worldPos.a < 0.01;

    // --- Rebuild the camera ray (pixel-center NDC, raster orientation:
    // texture row 0 is the top = clip-space y +1). ---
    float2 uv = (float2(dispatchThreadID) + 0.5) / float2(dim);
    float4 clipPos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    float4 viewPos = mul(clipPos, viewConstant.matClipToView);
    viewPos.xyz /= viewPos.w;
    float3 rayDirW =
        normalize(mul(float4(viewPos.xyz, 0.0), viewConstant.matViewToWorld).xyz);
    float3 camPosW =
        mul(float4(0.0, 0.0, 0.0, 1.0), viewConstant.matViewToWorld).xyz;

    // --- Sky (Hosek dome, if the scene has one) ---
    bool hasHosek = false;
    LightData dome;
    for (uint i = 0; i < lightCount; ++i) {
        LightData light = lightBuffer[i];
        if (light.type == uint(LightType::Dome) &&
            light.hosekStateIndex != 0) {  // 0 = dummy row = non-Hosek dome
            dome = light;
            hasHosek = true;
            break;
        }
    }

    float3 backgroundRadiance = float3(0.0);
    float3 skyAmbient = float3(0.0);
    if (hasHosek) {
        HosekSkyState sky = hosekStateBuffer[dome.hosekStateIndex];
        float3 localDir =
            normalize(mul(float4(rayDirW, 0.0), dome.transMat).xyz);
        backgroundRadiance =
            evalHosekSkyRadiance(sky, localDir, dome.dirW, dome.intensity);

        // Ambient field: evaluate the sky once in a sun-ward-blend direction,
        // fold the HG phase toward the camera into it. (Uniform-radiance
        // hemisphere integral: integral L(omega) f(omega) ds ~= L_bar with
        // directional variation from the mean direction.)
        float3 sunLocal = hosekResolveSunDir(dome.dirW);
        float3 ambDir = normalize(sunLocal * 0.5 + float3(0.0, 1.0, 0.0));
        float3 ambRadiance =
            evalHosekSkyRadiance(sky, ambDir, dome.dirW, dome.intensity);
        float ambPhase =
            evalHenyeyGreensteinFog(dot(ambDir, -rayDirW), phaseG);
        skyAmbient = ambRadiance * ambPhase;
    }

    // --- March the height fog ---
    float tScene = isBackground
        ? maxDistance
        : min(length(worldPos.xyz - camPosW), maxDistance);

    float3 vTowardCamera = -rayDirW;
    float3 inscatter = float3(0.0);
    float transmittance = 1.0;

    const int STEPS = 32;
    float dt = tScene / float(STEPS);
    for (int s = 0; s < STEPS; ++s) {
        float tMid = (float(s) + 0.5) * dt;
        float3 p = camPosW + rayDirW * tMid;

        float sigmaT = fogSigmaT(p);
        float od = sigmaT * dt;
        if (od < 1e-7)
            continue;

        // Exact per-segment integral for constant radiance across the step:
        // integral sigma_s * L * exp(-sigma_t t) dt = albedo * L * (1 - e^-od).
        float segT = exp(-od);
        float3 Lr = fogInscatterRadiance(p, vTowardCamera, skyAmbient);
        inscatter += transmittance * scatterAlbedo * Lr * (1.0 - segT);
        transmittance *= segT;
    }

    // --- Composite ---
    float3 sceneRadiance =
        isBackground ? (skyBackground != 0u ? backgroundRadiance
                                            : float3(0.0))
                     : sceneColor.rgb;
    float3 result = sceneRadiance * transmittance + inscatter;

    output[dispatchThreadID] = float4(result, 1.0);
}
