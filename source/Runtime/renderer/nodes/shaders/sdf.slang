#define PNANOVDB_HLSL
#include "PNanoVDB.h"
#include "utils/random.slangh"
#include "utils/ray.slang"

RWStructuredBuffer<uint> random_seeds;
RWTexture2D<float4> output;
StructuredBuffer<uint2> inPixelTarget;
StructuredBuffer<RayInfo> rays;

// Access NanoVDB buffer from bindless descriptor table entry 0
StructuredBuffer<uint> t_BindlessBuffers[];

struct RayMarchPayload {
    float3 color;
    bool hit;
};

// SDF sampling function using NanoVDB with trilinear interpolation
float sampleSDF(float3 position)
{
    // Access the NanoVDB buffer from entry 0
    pnanovdb_buf_t buf = t_BindlessBuffers[0];

    // Create grid and tree handles
    pnanovdb_grid_handle_t gridHdl = { 0 };
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, gridHdl);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    // Initialize read accessor
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);

    // Transform world position to index space using the grid's transform
    pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, gridHdl);
    float3 indexPos = pnanovdb_map_apply_inverse(buf, map, position);

    // Trilinear interpolation for smooth sampling
    float3 ijk = indexPos;  // Keep as float for interpolation
    int3 ijk0 = int3(floor(ijk));
    int3 ijk1 = ijk0 + int3(1, 1, 1);
    float3 f = ijk - float3(ijk0);

    // Sample 8 corner values
    float v000 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk0.x, ijk0.y, ijk0.z)));
    float v001 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk0.x, ijk0.y, ijk1.z)));
    float v010 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk0.x, ijk1.y, ijk0.z)));
    float v011 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk0.x, ijk1.y, ijk1.z)));
    float v100 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk1.x, ijk0.y, ijk0.z)));
    float v101 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk1.x, ijk0.y, ijk1.z)));
    float v110 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk1.x, ijk1.y, ijk0.z)));
    float v111 = pnanovdb_read_float(
        buf,
        pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, buf, acc, int3(ijk1.x, ijk1.y, ijk1.z)));

    // Trilinear interpolation
    float v00 = lerp(v000, v100, f.x);
    float v01 = lerp(v001, v101, f.x);
    float v10 = lerp(v010, v110, f.x);
    float v11 = lerp(v011, v111, f.x);

    float v0 = lerp(v00, v10, f.y);
    float v1 = lerp(v01, v11, f.y);

    return lerp(v0, v1, f.z);
}

// Ray marching implementation
float3 rayMarch(float3 rayOrigin, float3 rayDirection, out bool hit)
{
    const float maxDistance = 200.0;
    const float epsilon = 0.001;
    const int maxSteps = 512*2;

    float totalDistance = 0.0;
    hit = false;

    for (int i = 0; i < maxSteps; i++) {
        float3 currentPos = rayOrigin + rayDirection * totalDistance;
        float distance = sampleSDF(currentPos);

        if (distance < epsilon) {
            hit = true;
            return currentPos;
        }

        totalDistance += distance;

        if (totalDistance > maxDistance) {
            break;
        }
    }

    return rayOrigin + rayDirection * totalDistance;
}

// Calculate normal using finite differences
float3 calculateNormal(float3 position)
{
    const float h = 0.001;  // Smaller step for better precision
    float3 normal = float3(
        sampleSDF(position + float3(h, 0, 0)) -
            sampleSDF(position - float3(h, 0, 0)),
        sampleSDF(position + float3(0, h, 0)) -
            sampleSDF(position - float3(0, h, 0)),
        sampleSDF(position + float3(0, 0, h)) -
            sampleSDF(position - float3(0, 0, h)));
    return normalize(normal);
}

// Simple shading function
float3 shade(float3 position, float3 normal, float3 viewDir)
{
    // Basic diffuse lighting
    float3 lightDir = normalize(float3(1, 1, 1));
    float diffuse = max(0.0, dot(normal, lightDir));

    // Simple material color
    float3 baseColor = float3(0.8, 0.6, 0.4);

    // Add some ambient
    float3 ambient = float3(0.1, 0.1, 0.2);

    return baseColor * diffuse + ambient;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = inPixelTarget[DispatchRaysIndex().x];
    RayInfo ray = rays[DispatchRaysIndex().x];

    RayMarchPayload payload;
    payload.color = float3(0, 0, 0);
    payload.hit = false;

    // Perform ray marching
    bool hit;
    float3 hitPosition = rayMarch(ray.Origin, ray.Direction, hit);

    if (hit) {
        // Calculate normal and shade
        float3 normal = calculateNormal(hitPosition);
        payload.color = shade(hitPosition, normal, -ray.Direction);
    }
    else {
        // Miss - use simple sky color
        float3 skyColor = lerp(
            float3(0.5, 0.7, 1.0),
            float3(0.8, 0.9, 1.0),
            ray.Direction.y * 0.5 + 0.5);
        payload.color = skyColor;
    }

    output[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void Miss(inout RayMarchPayload payload)
{
    // Simple sky gradient
    float3 rayDir = WorldRayDirection();
    float3 skyColor = lerp(
        float3(0.5, 0.7, 1.0), float3(0.8, 0.9, 1.0), rayDir.y * 0.5 + 0.5);
    payload.color = skyColor;
}
