// Compute shader to generate AABBs from capsule segments (endpoint pairs +
// radii). Mirrors compute_sphere_aabbs.slang for the debug bristle pipeline:
// each prim is the capsule around segment [A, B] with the given radius.

struct SphereAABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

ByteAddressBuffer g_VertexBuffer;
RWStructuredBuffer<SphereAABB> g_OutputAABBs;

cbuffer Params {
    uint g_SegmentCount;
    uint g_EndpointsAOffset;  // Byte offset to endpoint-A block (float3 each)
    uint g_EndpointsBOffset;  // Byte offset to endpoint-B block (float3 each)
    uint g_RadiiOffset;       // Byte offset to radii block (float each)
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint segmentIndex = dispatchThreadID.x;

    if (segmentIndex >= g_SegmentCount)
        return;

    float3 a = asfloat(g_VertexBuffer.Load3(g_EndpointsAOffset + segmentIndex * 12));
    float3 b = asfloat(g_VertexBuffer.Load3(g_EndpointsBOffset + segmentIndex * 12));
    float radius = asfloat(g_VertexBuffer.Load(g_RadiiOffset + segmentIndex * 4));

    SphereAABB aabb;
    aabb.minX = min(a.x, b.x) - radius;
    aabb.minY = min(a.y, b.y) - radius;
    aabb.minZ = min(a.z, b.z) - radius;
    aabb.maxX = max(a.x, b.x) + radius;
    aabb.maxY = max(a.y, b.y) + radius;
    aabb.maxZ = max(a.z, b.z) + radius;

    g_OutputAABBs[segmentIndex] = aabb;
}
