// Material BRDF Importance Sampling Test
// Samples from BRDF and divides by PDF to check if result is uniform
// For good importance sampling, (sample_weight / pdf) should be approximately
// constant

import callable_data;
#include "utils/random.slangh"
#include "utils/ray.slang"
import Scene.SceneTypes;
import Scene.BindlessMaterial;
import Scene.BindlessVertexBuffer;
import utils.Math.ShadingFrame;
import Utils.Math.MathHelpers;

RWStructuredBuffer<uint>
    importance_buffer;  // Uint buffer for atomic operations (float data stored
                        // as uint)
RaytracingAccelerationStructure SceneBVH;

struct ImportanceSampleConstants {
    float3 incident_direction;
    float _pad0;
    float2 uv_coord;
    uint material_id;
    uint resolution;
    uint num_samples;
};

ConstantBuffer<ImportanceSampleConstants> importance_params;

// Atomic float add using InterlockedCompareExchange
void AtomicAddFloat(uint index, float value)
{
    uint original_value = importance_buffer[index];
    uint new_value;

    while (true) {
        new_value = asuint(asfloat(original_value) + value);
        uint compare_result;
        InterlockedCompareExchange(
            importance_buffer[index],
            original_value,
            new_value,
            compare_result);

        if (compare_result == original_value)
            break;

        original_value = compare_result;
    }
}

float2 direction_to_texcoord(float3 dir, out float jacobian)
{
    float z = dir.z;

    if (z <= 0.0) {
        // Below hemisphere
        jacobian = 0.0;
        return float2(-1, -1);
    }

    float2 p = dir.xy;

    jacobian = max(z, 1e-6);

    // Map from [-1, 1] to [0, 1]
    return p * 0.5 + 0.5;
}

struct ImportancePayload {
    uint sample_count;
    bool valid;
};

[shader("raygeneration")]
void ImportanceSampleRayGen()
{
    uint2 dispatch_id = DispatchRaysIndex().xy;
    uint dispatch_dim = DispatchRaysDimensions().x;  // Assuming square dispatch
    uint sample_id = dispatch_id.y * dispatch_dim + dispatch_id.x;
    uint resolution = importance_params.resolution;

    // Each thread generates ONE sample
    // Initialize RNG seed uniquely per sample
    uint seed = sample_id * 1664525 + 1013904223;
    seed = random_unsigned(seed);

    // Incident direction (view direction) - keep in world space for material
    float3 world_wi = normalize(importance_params.incident_direction);

    // Create dummy vertex for material evaluation
    VertexInfo vertex;
    vertex.posW = float3(0, 0, 0);
    vertex.normalW = float3(0, 0, 1);      // Z-up in world space
    vertex.tangentW = float4(1, 0, 0, 1);  // X-axis tangent
    vertex.texC = importance_params.uv_coord;
    vertex.subsetMaterialID = -1;

    // Get material
    uint material_id = importance_params.material_id;
    var material_header = materialHeaderBuffer[material_id];

    // Prepare callable data for sampling
    CallableData callData;
    callData.vertexInfo = vertex;
    callData.L =
        float3(0, 0, 1);    // Dummy L, will be replaced by sampled direction
    callData.V = world_wi;  // View direction in WORLD space (material will
                            // convert to local)
    callData.color = float4(0, 0, 0, 1);
    callData.seed = seed;
    callData.eta_flipped = 0;
    callData.materialBlobID = material_header.material_blob_id;

    // Call material to generate sample
    CallShader(material_header.material_type_id, callData);

    // Extract sampled direction
    // The material returns direction in WORLD space (after sf.fromLocal)
    // In our setup with normal=(0,0,1), world space = local space
    float3 world_sampled = callData.sampledDir;
    float3 local_sampled =
        world_sampled;  // world = local in our coordinate system

    // Validate sample: must have valid PDF and be in upper hemisphere
    // Invalid samples (pdf=0 or below hemisphere) are rejected and don't count
    if (callData.pdf > 1e-8 && local_sampled.z > 0.0) {
        // Convert sampled direction to texture coordinate
        float jacobian;
        float2 uv = direction_to_texcoord(local_sampled, jacobian);

        if (all(uv >= 0.0) && all(uv <= 1.0) && jacobian > 0.0) {
            // Convert to pixel coordinates
            uint2 pixel = uint2(uv * float(resolution));
            pixel = min(pixel, uint2(resolution - 1, resolution - 1));

            // Compute importance sampling weight: throughput / pdf
            // Note: PDF is in solid angle, but we need PDF in texture space
            // pdf_texture = pdf_solid_angle * jacobian
            // So weight_texture = throughput / pdf_texture = throughput / (pdf
            // * jacobian) We use a scaling factor for better visualization
            float pdf_texture = callData.pdf;
            float weight = 0.01f / pdf_texture * jacobian;

            // Atomically accumulate using AtomicAddFloat
            // Each pixel has 2 floats: [weight_accumulator, sample_count]
            uint pixel_index = pixel.y * resolution + pixel.x;

            AtomicAddFloat(pixel_index * 2 + 0, weight);
            AtomicAddFloat(pixel_index * 2 + 1, 1.0);
        }
    }
}

[shader("closesthit")]
void ImportanceHit(
    inout ImportancePayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    // Not used
}

[shader("miss")]
void ImportanceMiss(inout ImportancePayload payload)
{
    // Not used
}
