// Compute shader to generate AABBs from sphere positions and radii

struct SphereAABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

ByteAddressBuffer g_VertexBuffer;
RWStructuredBuffer<SphereAABB> g_OutputAABBs;

cbuffer Params {
    uint g_SphereCount;
    uint g_PositionsOffset;  // Byte offset to positions
    uint g_RadiiOffset;      // Byte offset to radii
};

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint sphereIndex = dispatchThreadID.x;

    if (sphereIndex >= g_SphereCount)
        return;

    // Read position (3 floats)
    uint posOffset = g_PositionsOffset + sphereIndex * 12;  // 3 * sizeof(float)
    float3 center = asfloat(g_VertexBuffer.Load3(posOffset));

    // Read radius (1 float)
    uint radiusOffset = g_RadiiOffset + sphereIndex * 4;  // sizeof(float)
    float radius = asfloat(g_VertexBuffer.Load(radiusOffset));

    SphereAABB aabb;
    aabb.minX = center.x - radius;
    aabb.minY = center.y - radius;
    aabb.minZ = center.z - radius;
    aabb.maxX = center.x + radius;
    aabb.maxY = center.y + radius;
    aabb.maxZ = center.z + radius;

    g_OutputAABBs[sphereIndex] = aabb;
}
