// deferred_direct_lighting.cs.slang
//
// Deferred direct-lighting compute pass for the rasterization pipeline.
//
// Consumes the G-Buffer produced by rasterize.*.slang, reconstructs the surface
// material (decode_material_surface), and accumulates direct lighting over all
// non-dome analytic lights — the raster analogue of the path tracer's
// direct-lighting step, minus shadow rays (deferred has no TLAS).
//
// STATUS: the full material-decode + per-material-BSDF + light loop is wired up
// below, BUT reading back the per-pixel G-Buffer SRVs from this compute pass
// currently causes a device-removed crash at dispatch time (the rasterize pass
// alone renders a correct G-Buffer; reading gWorldPos/gNormal/etc. as SRVs in
// the subsequent compute pass removes the device). This is under investigation
// — likely a resource-state/keepInitialState interaction across the
// graphics→compute command-list boundary. Until resolved, the pass outputs a
// debug constant so the graph runs end-to-end.
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
Texture2D<float4>  gWorldPos        : register(t0);
Texture2D<float2>  gTexcoord        : register(t1);
Texture2D<float4>  gAlbedo          : register(t2);
Texture2D<float4>  gMetallicRoughness : register(t3);
Texture2D<float4>  gNormal          : register(t4);
Texture2D<float>   gMaterialID      : register(t5);

// ---[ Scene bindings ]---
// materialBlobBuffer (t9) / materialHeaderBuffer (t7) are declared by
// Scene.MaterialEvaluation (imported above); do not redeclare here.
StructuredBuffer<LightData> lightBuffer : register(t6);
StructuredBuffer<uint>     materialTypeLUT : register(t8);

ConstantBuffer<PlanarViewConstants> viewConstant : register(b0);

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

    // TODO(deferred): the G-Buffer SRV reads below crash the device at dispatch.
    // Outputting a debug gradient (proportional to pixel coord) so the graph
    // runs end-to-end and the rasterize G-Buffer can be validated via its own
    // present path. Restore the material/lighting path once the SRV-read crash
    // is resolved (see file header).
    float2 uv = float2(dispatchThreadID) / float2(dim);
    output[dispatchThreadID] = float4(uv.x, uv.y, 0.25, 1.0);
}
