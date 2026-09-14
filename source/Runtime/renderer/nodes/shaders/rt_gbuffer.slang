// rt_gbuffer.slang -- standalone raytracing primary-ray G-buffer node.
//
// Launches ONE primary ray per pixel through the same geometry path as the
// path tracer (same TLAS, same hit groups' get_interpolated_vertex
// interpolation, same facing flip) and records the hit's world normal,
// position, texcoords, and tangent frame. This is the ground-truth track
// used to cross-check the rasterize G-buffer.
//
// Deliberately geometry-only: no materials, no lights, no volume hit
// groups. A volume instance routes to hit-group slot 6/7 which this
// context never announces -- the SBT hole-fill makes that inert (the slot
// is never routed to by triangle meshes; volume instances are unsupported
// in this node).

#include "utils/ray.slang"
import Scene.SceneTypes;
import Scene.BindlessVertexBuffer;

RaytracingAccelerationStructure SceneBVH;

struct RtgConstants
{
    uint width;
    uint height;
};

ConstantBuffer<RtgConstants> rtgConstants;

// Written by RayGen as linear-indexed float4 arrays. Structured buffers
// (not RWTextures) because RT PSOs here are proven with structured-buffer
// UAVs; the node reads them back and uploads to presentable textures.
RWStructuredBuffer<float4> gbuffer_normal;
RWStructuredBuffer<float4> gbuffer_position;
RWStructuredBuffer<float4> gbuffer_uv;
RWStructuredBuffer<float4> gbuffer_tangent;

StructuredBuffer<uint2> inPixelTarget;
StructuredBuffer<RayInfo> rays;

struct RtgPayload
{
    VertexInfo vertex;
    bool hit;
};

[shader("closesthit")]
void ClosestHit(
    inout RtgPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit = true;

    VertexInfo vertex = get_interpolated_vertex(
        InstanceID(),
        PrimitiveIndex(),
        float2(attribs.barycentrics.x, attribs.barycentrics.y));

    // Same facing flip as the path tracer's ClosestHit, so the recorded
    // normal is exactly the shading normal a PT primary hit would use.
    if (dot(WorldRayDirection(), vertex.normalW) >= 0.0) {
        vertex.normalW   = -vertex.normalW;
        vertex.tangentW  = -vertex.tangentW;
        vertex.faceNormalW = -vertex.faceNormalW;
    }

    payload.vertex = vertex;
}

[shader("miss")]
void Miss(inout RtgPayload payload)
{
    payload.hit = false;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = inPixelTarget[DispatchRaysIndex().x];
    RayInfo ray = rays[DispatchRaysIndex().x];

    RtgPayload payload;
    payload.hit = false;

    RayDesc current_ray = get_ray_desc(ray);
    current_ray.TMax = 50000.0;

    TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 1, 0, current_ray, payload);

    // Row-major linear index -- matches the host's texture upload.
    // (The pixel_targets buffer itself is column-major; see raygen notes
    // in path_tracing.slang.)
    const uint li = pixel.y * rtgConstants.width + pixel.x;
    if (payload.hit) {
        gbuffer_normal[li] =
            float4(normalize(payload.vertex.normalW) * 0.5 + 0.5, 1.0);
        gbuffer_position[li] = float4(payload.vertex.posW, 1.0);
        gbuffer_uv[li]       = float4(payload.vertex.texC, float2(0.0, 1.0));
        gbuffer_tangent[li]  = float4(payload.vertex.tangentW.xyz,
                                     payload.vertex.tangentW.w);
    }
    else {
        gbuffer_normal[li]   = float4(0.0, 0.0, 0.0, 0.0);
        gbuffer_position[li] = float4(0.0, 0.0, 0.0, 0.0);
        gbuffer_uv[li]       = float4(0.0, 0.0, 0.0, 0.0);
        gbuffer_tangent[li]  = float4(0.0, 0.0, 0.0, 0.0);
    }
}
