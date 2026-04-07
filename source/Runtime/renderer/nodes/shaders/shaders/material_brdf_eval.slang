// Material BRDF Eval and PDF Visualization Shader
// Computes BRDF eval and PDF for all outgoing directions on hemisphere

import callable_data;
#include "utils/random.slangh"
#include "utils/ray.slang"
import Scene.SceneTypes;
import Scene.BindlessMaterial;
import Scene.BindlessVertexBuffer;
import utils.Math.ShadingFrame;
import Utils.Math.MathHelpers;

RWTexture2D<float4> eval_output;
RWTexture2D<float4> pdf_output;
RaytracingAccelerationStructure SceneBVH;

struct AnalysisConstants {
    float3 incident_direction;  // Wi in world space
    float _pad0;
    float2 uv_coord;
    uint material_id;
    uint resolution;
};

ConstantBuffer<AnalysisConstants> analysis_params;

// Convert 2D texture coordinate to 3D direction using equal-area projection
// Standard Lambert azimuthal equal-area projection
float3 texcoord_to_direction(float2 uv)
{
    // Equal-area projection (Lambert azimuthal)
    // Maps unit disk to hemisphere uniformly by area
    float2 p = uv * 2.0 - 1.0;  // Map to [-1, 1]

    float r_squared = dot(p, p);
    if (r_squared > 1.0) {
        // Outside hemisphere, return zero direction
        return float3(0, 0, -1);
    }

    // Standard Lambert azimuthal equal-area projection:
    // For unit disk mapped to unit hemisphere:
    // x = p.x, y = p.y, z = sqrt(1 - p²)
    // This automatically gives unit vectors: x² + y² + z² = p² + (1-p²) = 1
    float z = sqrt(1.0 - r_squared);

    // This is already a unit vector
    return float3(p.x, p.y, z);
}

// Compute Jacobian for the equal-area projection
// This is needed to convert between direction probability and texture space probability
float compute_jacobian(float3 dir)
{
    float z = dir.z;
    if (z <= 0.0) return 0.0;

    // For Lambert azimuthal equal-area projection:
    // The Jacobian |dω/dA| = 2π (constant)
    // This means pdf_texture = pdf_solid_angle * 2π
    return 2.0 * 3.14159265359;
}

struct EvalPayload {
    float3 brdf_value;
    float pdf_value;
    bool valid;
};

[shader("raygeneration")]
void EvalPdfRayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint resolution = analysis_params.resolution;

    if (any(pixel >= resolution)) {
        return;
    }

    // Convert pixel to UV [0, 1]
    float2 uv = (float2(pixel) + 0.5) / float(resolution);

    // Convert UV to outgoing direction (local space, z-up hemisphere)
    // texcoord_to_direction now returns unit vectors directly
    float3 local_wo = texcoord_to_direction(uv);

    // Check if direction is valid (inside hemisphere)
    if (local_wo.z <= 0.0) {
        eval_output[pixel] = float4(0, 0, 0, 0);
        pdf_output[pixel] = float4(0, 0, 0, 0);
        return;
    }

    // Incident direction (view direction)
    // In our setup: normal=(0,0,1), so world space = local space
    float3 world_wi = normalize(analysis_params.incident_direction);
    float3 world_wo = local_wo;  // Since our coordinate system is world = local

    // Debug: For one pixel, check if directions are varying
    if (pixel.x == 256 && pixel.y == 256) {
        // Center pixel - log to see what we're passing
        // This would require a debug buffer, but we can output to RGB channels for visualization
    }

    // Create a dummy vertex info for material evaluation
    VertexInfo vertex;
    vertex.posW = float3(0, 0, 0);
    vertex.normalW = float3(0, 0, 1);  // Z-up in world space
    vertex.tangentW = float4(1, 0, 0, 1);  // X-axis tangent
    vertex.texC = analysis_params.uv_coord;
    vertex.subsetMaterialID = -1;

    // Get material
    uint material_id = analysis_params.material_id;
    var material_header = materialHeaderBuffer[material_id];

    // Prepare callable data
    CallableData callData;
    callData.vertexInfo = vertex;
    callData.L = world_wo;   // Outgoing/Light direction in WORLD space
    callData.V = world_wi;   // View/Incident direction in WORLD space
    callData.color = float4(0, 0, 0, 1);
    callData.seed = 12345;  // Dummy seed
    callData.eta_flipped = 0;
    callData.materialBlobID = material_header.material_blob_id;

    // Call material evaluation
    CallShader(material_header.material_type_id, callData);

    // Extract BRDF value
    // Note: fetch_shader_data returns BRDF in 'color' output (Surface_out.color)
    // The 'pdf' and 'throughput' from fetch_shader_data are for the sampled direction,
    // not for our input L direction, so they're not useful for evaluation
    float3 brdf = callData.color.rgb;

    // Store results
    // RGB = BRDF value, A = cos(theta) for reference
    eval_output[pixel] = float4(brdf, local_wo.z);

    // PDF output: We cannot get meaningful PDF from MaterialX for arbitrary L direction
    // MaterialX's fetch_shader_data only returns PDF for importance-sampled directions
    // Store BRDF again for now (or could compute theoretical PDF if we had BRDF parameters)
    pdf_output[pixel] = float4(brdf, 1.0);
}

[shader("closesthit")]
void EvalHit(inout EvalPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    // Not used for this shader
    payload.valid = false;
}

[shader("miss")]
void EvalMiss(inout EvalPayload payload)
{
    // Not used for this shader
    payload.valid = false;
}
