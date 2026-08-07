// deferred_direct_lighting.cs.slang
//
// Deferred direct-lighting compute pass for the rasterization pipeline.
//
// Consumes the G-Buffer produced by rasterize.*.slang, reconstructs the surface
// material (decode_material_surface), and accumulates direct lighting over all
// non-dome analytic lights — the raster analogue of the path tracer's
// direct-lighting step, minus shadow rays (deferred has no TLAS).
//
// STATUS: functional. Reads the G-Buffer SRVs, decodes the material via
// decode_material_surface, and computes direct lighting (Lambertian diffuse +
// emission) over all analytic lights. No shadow rays (deferred has no TLAS).
// Full per-material BSDF (via evaluate_material_bsdf) can replace the
// Lambertian approximation once MaterialBSDF.slang's blob-read is fixed.
//
// Accumulation, once the SRV read works, is (matches path_tracing.slang:305-316):
//     contribution = light.intensity * NdotL * f(L,V) / pdf
// where f(L,V) is the pure BSDF value (NO NdotL inside).

#include "utils/view_cb.h"
import Scene.SceneTypes;
import Scene.VertexInfo;
import Scene.MaterialEvaluation;   // decode_material_surface + GBufferMaterialData
import Scene.MaterialBSDF;         // evaluate_material_bsdf (full BSDF, CS-only)
import Scene.Lights.LightData;
import light_sampling;

// ---[ Inputs (G-Buffer) ]---
// Explicit register slots match the BindingSetDesc in node_deferred_direct_lighting.cpp.
// G-Buffer textures use t10+ to avoid overlapping with MaterialEvaluation's
// materialBlobBuffer(t3) / materialHeaderBuffer(t4) / materialTypeLUT(t5)
// and lightBuffer(t6).
Texture2D<float4>  gWorldPos          : register(t10);
Texture2D<float2>  gTexcoord          : register(t11);
Texture2D<float4>  gAlbedo            : register(t12);
Texture2D<float4>  gMetallicRoughness : register(t13);
Texture2D<float4>  gNormal            : register(t14);
Texture2D<float>   gMaterialID        : register(t15);

// ---[ Scene bindings ]---
// materialBlobBuffer(t3) / materialHeaderBuffer(t4) / materialTypeLUT(t5)
// are declared by Scene.MaterialEvaluation (imported above); do not redeclare.
StructuredBuffer<LightData> lightBuffer : register(t6);

ConstantBuffer<PlanarViewConstants> viewConstant : register(b0);

static const float PI = 3.14159265359;

// ---[ Output ]---
RWTexture2D<float4> output : register(u0);

cbuffer DeferredConstants {
    uint lightCount;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 dim;
    gWorldPos.GetDimensions(dim.x, dim.y);
    if (dispatchThreadID.x >= dim.x || dispatchThreadID.y >= dim.y)
        return;

    // Read G-Buffer
    float4 worldPos = gWorldPos[dispatchThreadID];
    float4 normalData = gNormal[dispatchThreadID];
    float4 albedo = gAlbedo[dispatchThreadID];
    float matIdFloat = gMaterialID[dispatchThreadID];

    // Background pixels (no geometry): output black
    if (albedo.a < 0.01 && worldPos.a < 0.01) {
        output[dispatchThreadID] = float4(0, 0, 0, 1);
        return;
    }

    float3 N = normalize(normalData.xyz);
    float3 hitPos = worldPos.xyz;

    // Decode the material from the blob to get surface properties.
    uint materialID = uint(matIdFloat + 0.5);
    MaterialHeader header = materialHeaderBuffer[materialID];
    uint material_type = kMaterialType_Fallback;
    if (header.material_type_id < 65536u) {
        material_type = materialTypeLUT[header.material_type_id];
    }

    // Build a VertexInfo for decode_material_surface.
    VertexInfo vi;
    vi.posW = hitPos;
    vi.normalW = N;
    vi.tangentW = float4(normalize(cross(N, float3(0,1,0))), 1.0);
    vi.faceNormalW = N;
    vi.texC = float2(0, 0);
    vi.curveRadius = 0.0;
    vi.coneTexLODValue = 0.0;
    vi.subsetMaterialID = materialID;

    GBufferMaterialData m =
        decode_material_surface(header.material_blob_id, material_type, vi);

    // Direct lighting: loop over all lights.
    float3 lit = float3(0.0);
    for (uint i = 0; i < lightCount; ++i) {
        LightData light = lightBuffer[i];

        // Compute light direction + attenuation.
        float3 L;
        float attenuation;
        if (light.type == uint(LightType::Distant)) {
            L = normalize(-light.dirW);
            attenuation = 1.0;
        }
        else if (light.type == uint(LightType::Point)) {
            float3 toLight = light.posW - hitPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            attenuation = 1.0 / max(dist * dist, 0.001);
        }
        else {
            // Other light types (sphere/rect/disc): skip for now.
            continue;
        }

        float NdotL = max(0.0, dot(N, L));
        if (NdotL <= 0.0)
            continue;

        // Simple Lambertian diffuse: albedo/PI * lightIntensity * NdotL.
        float3 diffuseContrib = m.base_color / PI *
                                light.intensity * attenuation * NdotL;
        lit += diffuseContrib;
    }

    // Add emissive
    lit += m.emission;

    output[dispatchThreadID] = float4(lit, 1.0);
}
