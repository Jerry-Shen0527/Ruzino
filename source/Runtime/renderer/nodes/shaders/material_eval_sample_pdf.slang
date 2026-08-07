// material_eval_sample_pdf.slang

#include "matrix_op.h"
#include "utils/HitObject.h"
#include "utils/random.slangh"
#include "utils/view_cb.h"

import Scene.SceneTypes;
import utils.Math.MathHelpers;
import callable_data;
RaytracingAccelerationStructure SceneBVH;

StructuredBuffer<HitObjectInfo> hitObjects;
StructuredBuffer<uint2> in_PixelTarget;

import Scene.BindlessVertexBuffer;

RWStructuredBuffer<uint2> PixelTarget;
RWStructuredBuffer<float4> Eval;
RWStructuredBuffer<RayInfo> Sample;
RWStructuredBuffer<float> Weight;
RWStructuredBuffer<float> Pdf;
RWStructuredBuffer<uint> random_seeds;

import Scene.Material.MaterialData;
import utils.Math.ShadingFrame;
import Scene.BindlessMaterial;

struct Payload {
    uint seed;
};

struct Attributes {
    float2 barycentric;
};

// Ray generation shader
[shader("raygeneration")]
void RayGen()
{
    int id = DispatchRaysIndex().x;

    HitObjectInfo hitObject = hitObjects[id];

    RayDesc ray_desc = get_ray_desc(hitObject.rays);
    Payload payload;
    payload.seed = random_seeds[DispatchRaysIndex().x];

// #define USE_SER
#ifdef USE_SER
    HitObject object = HitObject.MakeHit<Attributes>(
        SceneBVH,
        hitObject.InstanceIndex,
        hitObject.GeometryIndex,
        hitObject.PrimitiveIndex,
        hitObject.HitKind,
        hitObject.RayContributionToHitGroupIndex,
        hitObject.MultiplierForGeometryContributionToHitGroupIndex,
        ray_desc,
        { hitObject.attributes });

    ReorderThread(object);

    object.Invoke(SceneBVH, object, payload);
#else
    // If not using SER, we launch the ray, like a normal hlsl shader
    TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray_desc, payload);
#endif
    random_seeds[DispatchRaysIndex().x] = payload.seed;
}

struct ShadowPayload {
    float3 shadow_mask;
};

// Closest hit shader
[shader("closesthit")]
void ClosestHit(
    inout Payload payload: SV_RayPayload,
    Attributes attrib: SV_IntersectionAttributes)
{
    uint id = DispatchRaysIndex().x;
    PixelTarget[id] = in_PixelTarget[id];

    var instance_id = InstanceID();
    var prim_id = PrimitiveIndex();
    VertexInfo vertex =
        get_interpolated_vertex(instance_id, prim_id, attrib.barycentric);

    if (dot(WorldRayDirection(), vertex.normalW) >= 0.0) {
        vertex.normalW = (-vertex.normalW);
    }

    var seed = payload.seed;

    // Calculate ambient occlusion
    ShadowPayload shadowPayload;
    shadowPayload.shadow_mask = float3(1, 1, 1);
    float pdf;
    float3 ray_dir =
        sample_cosine_hemisphere_concentric(random_float2(seed), pdf);

    float3 light_pos = float3(4, 4, 9) / 1.3;

    bool valid;
    ShadingFrame sf =
        ShadingFrame.createSafe(vertex.normalW, float4(0, 0, 0, 1), valid);

    vertex.tangentW.xyz = sf.T;

    var world_ray_dir = sf.fromLocal(ray_dir);

    // world_ray_dir = -float3(0, 0.2, -1);
    // world_ray_dir = normalize(light_pos - vertex.posW);

    RayDesc shadow_ray_desc;
    shadow_ray_desc.Origin = vertex.posW + vertex.normalW * 0.001;
    shadow_ray_desc.Direction = world_ray_dir;
    shadow_ray_desc.TMin = 0.01;
    shadow_ray_desc.TMax = 1000.0;

    TraceRay(
        SceneBVH, RAY_FLAG_NONE, 0xFF, 1, 1, 1, shadow_ray_desc, shadowPayload);
    var result = shadowPayload.shadow_mask;

    var material_id = instanceDescBuffer[instance_id].materialID;

    if (vertex.subsetMaterialID != -1) {
        material_id = vertex.subsetMaterialID;
    }

    // Call the callable shader to get color based on material_id
    CallableData callData;
    callData.vertexInfo = vertex;

    callData.L = world_ray_dir;
    callData.V = -WorldRayDirection();
    callData.color = float4(0, 0, 0, 1);
    callData.materialID = material_id;
    CallShader(material_id, callData);
    result *= callData.color.rgb * 3.14 * 2 * 4;
    // result = float3(callData.color.w);

    if (isnan(result.x))
        result = float3(1, 0, 1);

    // result = float3(vertex.subsetMaterialID) / 35.0f;
    //  result = material_id / 35.0f;
    //
    Eval[id] = float4(result, 1);
    // Eval[id] = float4(0.5 + 0.5 * vertex.tangentW.xyz, 1);
    // * float4(hitObjects[id].rays.throughput.toFloat3(), 1.0);

    // Acquire the material information here.

    payload.seed = seed;
}

// Miss shader
[shader("miss")]
void Miss(inout Payload payload: SV_RayPayload)
{
    uint id = DispatchRaysIndex().x;
    PixelTarget[id] = in_PixelTarget[id];
    Eval[id] = float4(1, 0, 1, 1);
}

[shader("closesthit")]
void ShadowHit(
    inout ShadowPayload payload: SV_RayPayload,
    Attributes attrib: SV_IntersectionAttributes)
{
    payload.shadow_mask = float3(0, 0, 0);
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload: SV_RayPayload)
{
    payload.shadow_mask = float3(1, 1, 1);
}

