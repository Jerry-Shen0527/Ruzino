import callable_data;
#include "utils/random.slangh"
#include "utils/ray.slang"
import Scene.SceneTypes;
import Scene.BindlessMaterial;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import Scene.BindlessVertexBuffer;

import Scene.StandardSurface;
import Scene.UsdPreviewSurface;
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;

import pt_constants;
import pt_sample_lights;

RWStructuredBuffer<uint> random_seeds;
RWTexture2D<float4> output;
StructuredBuffer<uint2> inPixelTarget;
StructuredBuffer<RayInfo> rays;

struct RayPayload {
    // Geometry information from hit
    VertexInfo vertex;
    uint materialID;
    uint eta_flipped;

    // Ray state
    uint depth;
    uint seed;
    bool hit;  // Whether ray hit geometry (false means miss)
};

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = inPixelTarget[DispatchRaysIndex().x];
    RayInfo ray = rays[DispatchRaysIndex().x];

    RayPayload payload;
    payload.seed = random_seeds[DispatchRaysIndex().x];

    Spectrum output_color = Spectrum(0.0);

    const int spp = 1;
    const int max_depth = 20;

    ray.TMax = 50000.0;

    for (int i = 0; i < spp; i++) {
        Spectrum accumulated_color = Spectrum(0.0);
        Spectrum current_throughput = Spectrum(1.0);

        RayDesc current_ray = get_ray_desc(ray);
        current_ray.TMax = 50000.0;

        uint id = DispatchRaysIndex().x;

        // Iterative path tracing loop
        for (int depth = 0; depth < max_depth; depth++) {
            payload.depth = depth;
            payload.hit = false;

            // Trace ray to get geometry information
            TraceRay(
                SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 1, 0, current_ray, payload);
#ifdef ENABLE_SER
            // SER: Reorder after TraceRay - hit/miss is THE most important
            // criterion Bit 31: hit(1) vs miss(0) - highest priority to
            // eliminate divergence Bits 16-19: depth (secondary priority) Bits
            // 0-15: material ID (for hit) or 0 (for miss)
            uint reorderHint =
                (payload.hit ? (1u << 31) : 0u) |  // Hit/miss flag
                ((depth & 0xF) << 16) |            // Depth
                (payload.hit ? (payload.materialID & 0xFFFF)
                             : 0u);  // Material ID if hit
            ReorderThread(reorderHint, 32);
#endif

            // Check if ray missed
            if (!payload.hit) {
                // Environment lighting (same as Miss shader)
                float3 rayDir = current_ray.Direction;
                Spectrum envColor = Spectrum(0.0);

                // Check if we have a dome light
                bool hasDomeLight = false;
                LightData domeLight;

                for (uint lightIdx = 0; lightIdx < ptConstants.lightCount;
                     lightIdx++) {
                    LightData light = lightBuffer[lightIdx];
                    if (light.type == uint(LightType::Dome)) {
                        hasDomeLight = true;
                        domeLight = light;
                        break;
                    }
                }

                if (hasDomeLight) {
                    float3 localDir =
                        mul(float4(rayDir, 0.0), domeLight.transMat).xyz;
                    localDir = normalize(localDir);

#if USE_DOME_LIGHT_CALLABLE
                    DomeLightCallableData domeData;
                    domeData.rayDirection = rayDir;
                    domeData.localDirection = localDir;
                    domeData.envColor = Spectrum(0.0);
                    domeData.intensity = domeLight.intensity;
                    domeData.textureIndex = domeLight.textureIndex;
                    domeData.seed = payload.seed;

                    CallShader(ptConstants.domeLightCallableIndex, domeData);

                    envColor = domeData.envColor;
                    payload.seed = domeData.seed;
#else
                    if (domeLight.textureIndex >= 0) {
                        float phi = atan2(localDir.z, localDir.x);
                        float theta = acos(clamp(localDir.y, -1.0, 1.0));
                        float u = (phi + 3.14159265359) / (2.0 * 3.14159265359);
                        float v = theta / 3.14159265359;

                        float4 texColor =
                            t_BindlessTextures[domeLight.textureIndex]
                                .SampleLevel(samplers[1], float2(u, v), 0);
                        envColor = Spectrum(texColor.rgb * domeLight.intensity);
                    }
                    else {
                        envColor = Spectrum(domeLight.intensity);
                    }
#endif
                }
                else {
                    // No dome light — black background
                }

                accumulated_color =
                    add(accumulated_color, mul(current_throughput, envColor));
                break;  // Path terminated
            }

            // Process hit - material evaluation
            VertexInfo vertex = payload.vertex;

            // output_color = Spectrum(vertex.tangentW.xyz * 0.5 + 0.5);
            // output_color = Spectrum(float3(payload.eta_flipped, 0, 0));
            // break;
            uint material_id = payload.materialID;
            uint eta_flipped = payload.eta_flipped;

            // No material assigned — use fallback Lambertian eval (index 2).
            // Same structure as the regular material path below: sample a
            // (non-dome) light, eval the fallback BRDF, accumulate direct
            // lighting, update throughput, then continue bouncing. Dome light
            // is picked up automatically on the next-bounce miss. (Previously
            // this branch ran its own dome-skipping direct-lighting loop and
            // then `break`-terminated, so fallback objects went fully black
            // under dome-only lighting and received no indirect light.)
            if (material_id == uint(-1)) {
                // Sample a light source for direct lighting
                LightSample lightSample =
                    sampleLights(vertex.posW, vertex.faceNormalW, payload.seed);

                // Fallback BRDF eval (Lambertian albedo=0.8)
                EvalCallableData evalData;
                evalData.color = Spectrum(0.0);
                evalData.throughput = Spectrum(1.0);
                evalData.L = lightSample.direction;
                evalData.V = -current_ray.Direction;
                evalData.sampledDir = float3(0, 0, 0);
                evalData.pdf = 0.0;
                evalData.seed = payload.seed;
                evalData.eta_flipped = eta_flipped;
                evalData.material_params_index = id;
                evalData.vertexInfo = vertex;

                CallShader(2, evalData);

                float pdf = evalData.pdf;
                payload.seed = evalData.seed;

                if (pdf == 0.0) {
                    break;
                }

                // Add direct lighting contribution (same formula as regular path)
                if (lightSample.valid) {
                    Spectrum directContrib = div(
                        mul(Spectrum(lightSample.intensity *
                            max(0.0, dot(vertex.normalW, lightSample.direction))),
                            evalData.color),
                        lightSample.pdf);
                    accumulated_color = add(
                        accumulated_color, mul(current_throughput, directContrib));
                }

                // Update throughput for next bounce
                float3 ray_dir = evalData.sampledDir;
                bool isTransmission = dot(ray_dir, vertex.faceNormalW) < 0.0;
                current_throughput =
                    mul(current_throughput, div(evalData.throughput, pdf));

                // Russian Roulette path termination
                if (depth >= 3) {
                    float3 thr = current_throughput.toFloat3();
                    float rrProbability = min(0.95, max(thr.x, max(thr.y, thr.z)));
                    if (random_float(payload.seed) > rrProbability) {
                        break;
                    }
                    current_throughput = div(current_throughput, rrProbability);
                }

                // Prepare next ray — continue bouncing (dome/indirect picked up
                // via the miss path on subsequent bounces)
                float3 offset_normal =
                    isTransmission ? -vertex.faceNormalW : vertex.faceNormalW;
                float3 ray_offset = offset_normal * 1e-3f;

                current_ray.Origin = vertex.posW + ray_offset;
                current_ray.Direction = ray_dir;
                current_ray.TMin = 0.0;
                current_ray.TMax = 3000.0;
                continue;
            }

            var material_header = materialHeaderBuffer[material_id];

            // Sample a light source for direct lighting
            LightSample lightSample =
                sampleLights(vertex.posW, vertex.faceNormalW, payload.seed);

            // Step 1: Call material data fetch callable
            FetchCallableData fetchData;
            fetchData.vertexInfo = vertex;
            fetchData.materialBlobID = material_header.material_blob_id;
            fetchData.material_params_index = id;

            CallShader(
                ptConstants.materialFetchCallableBaseIndex +
                    material_header.material_type_id,
                fetchData);

            // Opacity presence test: with probability (1 - opacity) the ray
            // passes straight through the surface (no scattering, no direct
            // lighting), letting the path continue to whatever is behind.
            // This is the standard path-tracing treatment of opacity<1
            // (cutout/presence): the surface exists with probability opacity,
            // otherwise the ray ignores it. The BSDF layer still clamps
            // opacity<0.25 to fully transparent (UsdPreviewSurface/standard),
            // so this branch mainly handles the 0.25..1 range.
            FetchCallableData opacityFetchData;
            opacityFetchData.vertexInfo = vertex;
            opacityFetchData.materialBlobID = material_header.material_blob_id;
            opacityFetchData.material_params_index = 0;
            opacityFetchData.shader_type_id = 0;
            opacityFetchData.opacityColor = float3(1.0, 1.0, 1.0);
            CallShader(
                ptConstants.materialFetchCallableBaseIndex +
                    material_header.material_type_id +
                    ptConstants.materialOpacityCallableOffset,
                opacityFetchData);
            float surfaceOpacity = asfloat(opacityFetchData.material_params_index);
            if (random_float(payload.seed) > surfaceOpacity) {
                // Ray passes through: continue along the same direction, tinted
                // by the opacity color. The survival probability is
                // (1 - surfaceOpacity); divide by it to keep the estimate
                // unbiased (per-channel survival differs from the scalar draw,
                // so without the division colored opacity would be biased).
                float3 tint = float3(1.0, 1.0, 1.0) - opacityFetchData.opacityColor;
                float surviveProb = max(1.0 - surfaceOpacity, 1e-4);
                current_throughput = mul(
                    current_throughput, Spectrum(tint / surviveProb));
                current_ray.Origin =
                    vertex.posW + current_ray.Direction * 1e-3f;
                continue;
            }

            // Step 2: Call material evaluation callable
            EvalCallableData evalData;
            evalData.color = Spectrum(0.0);
            evalData.throughput = Spectrum(1.0);
            evalData.L = lightSample.direction;
            evalData.V = -current_ray.Direction;
            evalData.sampledDir = float3(0, 0, 0);
            evalData.pdf = 0.0;
            evalData.seed = payload.seed;
            evalData.eta_flipped = eta_flipped;
            evalData.material_params_index = id;
            evalData.vertexInfo = vertex;

            CallShader(fetchData.shader_type_id, evalData);

            float pdf = evalData.pdf;
            payload.seed = evalData.seed;

            // Check for invalid PDF
            if (pdf == 0.0) {
                break;
            }

            // Validate throughput
            if (any(isnan(evalData.throughput.toFloat3())) ||
                any(isinf(evalData.throughput.toFloat3()))) {
                accumulated_color = Spectrum(float3(1, 0, 0));  // Debug color
                break;
            }

            // Add direct lighting contribution
            if (lightSample.valid) {
                Spectrum directContrib = div(
                    mul(Spectrum(
                            lightSample.intensity *
                            max(0.0,
                                dot(vertex.normalW, lightSample.direction))),
                        evalData.color),
                    lightSample.pdf);
                accumulated_color = add(
                    accumulated_color, mul(current_throughput, directContrib));
            }

            // Update throughput for next bounce
            float3 ray_dir = evalData.sampledDir;
            bool isTransmission = dot(ray_dir, vertex.faceNormalW) < 0.0;

            current_throughput =
                mul(current_throughput, div(evalData.throughput, pdf));

            // Russian Roulette path termination
            if (depth >= 3) {
                float3 thr = current_throughput.toFloat3();
                float rrProbability = min(0.95, max(thr.x, max(thr.y, thr.z)));
                if (random_float(payload.seed) > rrProbability) {
                    break;
                }
                current_throughput = div(current_throughput, rrProbability);
            }

            // Prepare next ray
            float3 offset_normal =
                isTransmission ? -vertex.faceNormalW : vertex.faceNormalW;
            float3 ray_offset = offset_normal * 1e-3f;

            current_ray.Origin = vertex.posW + ray_offset;
            current_ray.Direction = ray_dir;
            current_ray.TMin = 0.0;
            current_ray.TMax = 3000.0;
        }

        output_color = add(output_color, accumulated_color);
    }
    output_color = div(output_color, float(spp));
    output[pixel] = float4(output_color.toFloat3(), 1.0);

    random_seeds[DispatchRaysIndex().x] = payload.seed;
}

[shader("closesthit")]
void ClosestHit(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit = true;

    uint instance_id = InstanceID();
    uint prim_id = PrimitiveIndex();

    // Get vertex information
    VertexInfo vertex = get_interpolated_vertex(
        instance_id,
        prim_id,
        float2(attribs.barycentrics.x, attribs.barycentrics.y));

    // Fix normal orientation based on geometric normal (faceNormalW)
    uint eta_flipped = 0;
    if (dot(WorldRayDirection(), vertex.normalW) >= 0.0) {
        vertex.normalW = -vertex.normalW;
        vertex.tangentW = -vertex.tangentW;
        vertex.faceNormalW = -vertex.faceNormalW;
        eta_flipped = 1;
    }

    // Get material ID
    uint material_id = instanceDescBuffer[instance_id].materialID;
    if (vertex.subsetMaterialID != -1) {
        material_id = vertex.subsetMaterialID;
    }

    // Store geometry information in payload
    payload.vertex = vertex;
    payload.materialID = material_id;
    payload.eta_flipped = eta_flipped;
}

[shader("closesthit")]
void SphereClosestHit(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit = true;
    uint instance_id = InstanceID();
    uint prim_id = PrimitiveIndex();

    // Compute hit point in world space
    float3 worldRayOrigin = WorldRayOrigin();
    float3 worldRayDirection = WorldRayDirection();
    float t = RayTCurrent();
    float3 hitPoint = worldRayOrigin + t * worldRayDirection;

    // Read sphere data from vertex buffer via bindless
    uint mesh_id = instanceDescBuffer[instance_id].geometryID;
    MeshDesc mesh_desc = meshDescBuffer[mesh_id];

    // Read position (3 floats at stride 12 bytes)
    uint position_offset = prim_id * 12;  // 3 * sizeof(float)
    float3 sphereCenterLocal = asfloat(
        t_BindlessBuffers[mesh_desc.bindlessIndex].Load3(
            mesh_desc.vbOffset + position_offset));

    // Transform sphere center to world space using 4x3 affine matrix
    float4x3 objToWorld = ObjectToWorld4x3();
    float3 sphereCenterWorld = objToWorld[0] * sphereCenterLocal.x +
                               objToWorld[1] * sphereCenterLocal.y +
                               objToWorld[2] * sphereCenterLocal.z +
                               objToWorld[3];

    // Compute geometric normal as direction from sphere center to hit point
    float3 normal = normalize(hitPoint - sphereCenterWorld);

    // Create vertex info for the sphere hit
    VertexInfo vertex;
    vertex.posW = hitPoint;
    vertex.normalW = normal;
    vertex.tangentW = float4(0, 0, 0, 1);
    vertex.faceNormalW = normal;
    vertex.texC = float2(0, 0);
    vertex.curveRadius = 0.0;
    vertex.coneTexLODValue = 0.0;
    vertex.subsetMaterialID = -1;

    // Fix normal orientation based on ray direction
    // For spheres, geometric normal and shading normal are the same
    uint eta_flipped = 0;
    if (dot(worldRayDirection, normal) >= 0.0) {
        vertex.normalW = -normal;
        vertex.faceNormalW = -normal;
        eta_flipped = 1;
    }

    // Get material ID from instance
    uint material_id = instanceDescBuffer[instance_id].materialID;

    // Store geometry information in payload
    payload.vertex = vertex;
    payload.materialID = material_id;
    payload.eta_flipped = eta_flipped;
}

[shader("intersection")]
void SphereIntersection()
{
    uint instance_id = InstanceID();
    uint prim_id = PrimitiveIndex();

    // Read sphere data from vertex buffer via bindless
    uint mesh_id = instanceDescBuffer[instance_id].geometryID;
    MeshDesc mesh_desc = meshDescBuffer[mesh_id];

    // Read position (3 floats at stride 12 bytes)
    uint position_offset = prim_id * 12;  // 3 * sizeof(float)
    float3 sphereCenterLocal = asfloat(
        t_BindlessBuffers[mesh_desc.bindlessIndex].Load3(
            mesh_desc.vbOffset + position_offset));

    // Transform sphere center to world space
    float4x3 objToWorld = ObjectToWorld4x3();
    float3 sphereCenterWorld = objToWorld[0] * sphereCenterLocal.x +
                               objToWorld[1] * sphereCenterLocal.y +
                               objToWorld[2] * sphereCenterLocal.z +
                               objToWorld[3];

    // Read radius - stored after all positions
    // mesh_desc.ibOffset stores the radius buffer offset (see points.cpp)
    uint radius_offset = prim_id * 4;  // sizeof(float)
    float radius = asfloat(
        t_BindlessBuffers[mesh_desc.bindlessIndex].Load(
            mesh_desc.ibOffset + radius_offset));

    // Ray-sphere intersection in world space
    float3 rayOrigin = WorldRayOrigin();
    float3 rayDir = WorldRayDirection();

    float3 oc = rayOrigin - sphereCenterWorld;
    float a = dot(rayDir, rayDir);
    float half_b = dot(oc, rayDir);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = half_b * half_b - a * c;

    if (discriminant >= 0.0) {
        float sqrtd = sqrt(discriminant);

        // Find the nearest valid intersection
        float t = (-half_b - sqrtd) / a;

        // Check if t is in valid range
        if (t >= RayTMin() && t <= RayTCurrent()) {
            // Report intersection with hit kind 0
            ReportHit(
                t,
                /*hitKind=*/0u,
                BuiltInTriangleIntersectionAttributes(float2(0, 0)));
        }
        else {
            // Try the other root (inside hit or backface)
            t = (-half_b + sqrtd) / a;
            if (t >= RayTMin() && t <= RayTCurrent()) {
                ReportHit(
                    t,
                    /*hitKind=*/0u,
                    BuiltInTriangleIntersectionAttributes(float2(0, 0)));
            }
        }
    }
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.hit = false;
}

[shader("closesthit")]
void ShadowHit(
    inout ShadowPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.isVisible = true;
    // payload.isVisible = false;
    // return;
    // Limit recursion depth to prevent infinite loops
    const uint MAX_SHADOW_OPACITY = 4;
    if (payload.recursionDepth >= MAX_SHADOW_OPACITY) {
        payload.isVisible = false;
        return;
    }

    uint instance_id = InstanceID();
    uint prim_id = PrimitiveIndex();

    // Get vertex information
    VertexInfo vertex = get_interpolated_vertex_simple(
        instance_id, prim_id, attribs.barycentrics);

    // Get material ID
    var material_id = instanceDescBuffer[instance_id].materialID;
    if (vertex.subsetMaterialID != -1) {
        material_id = vertex.subsetMaterialID;
    }

    // No material — treat as opaque (shadow ray is blocked)
    if (material_id == uint(-1)) {
        payload.isVisible = false;
        return;
    }

    var material_header = materialHeaderBuffer[material_id];

    // Optimized opacity fetch: Call the opacity-specific callable
    // This avoids full material parameter buffer access
    FetchCallableData opacityFetchData;
    opacityFetchData.vertexInfo = vertex;
    opacityFetchData.materialBlobID = material_header.material_blob_id;
    opacityFetchData.material_params_index =
        0;  // Will be filled with opacity value
    opacityFetchData.shader_type_id = 0;
    opacityFetchData.opacityColor = float3(1.0, 1.0, 1.0);

    // Call opacity fetch callable (uses fetch_shader_opacity internally)
    CallShader(
        ptConstants.materialFetchCallableBaseIndex +
            material_header.material_type_id +
            ptConstants.materialOpacityCallableOffset,
        opacityFetchData);

    // Extract opacity from material_params_index (reinterpreted as float)
    float opacity = asfloat(opacityFetchData.material_params_index);
    // Stochastic opacity test — symmetric with the main radiance loop's
    // presence pass-through (path_tracing.slang main loop). With probability
    // (1-opacity) the shadow ray passes through this surface and keeps
    // tracing, so a semi-transparent surface (opacity 0.5) blocks shadow
    // rays only ~50% of the time → casts a soft partial shadow instead of
    // the former hard binary cutoff at opacity<0.25. This replaces both the
    // dead `opacity_threshold = random_float(...)` (which consumed a random
    // number but never used it) and the `opacity < 0.25` binary test.
    if (random_float(payload.seed) >= opacity) {
        // Surface is transparent to this shadow ray — continue tracing
        // toward the light. If there's no distance left, the light is
        // effectively unobstructed.
        float3 rayOrigin = vertex.posW;
        float remainingDist = payload.rayTMax - RayTCurrent();
        if (remainingDist <= 0.001) {
            payload.isVisible = true;
            return;
        }

        RayDesc continuationRay;
        continuationRay.Origin = rayOrigin;
        continuationRay.Direction = WorldRayDirection();
        continuationRay.TMin = 0.001;
        continuationRay.TMax = remainingDist;

        ShadowPayload continuationPayload;
        continuationPayload.isVisible = true;
        continuationPayload.rayTMax = remainingDist;
        continuationPayload.recursionDepth = payload.recursionDepth + 1;
        continuationPayload.seed = payload.seed;

        TraceRay(
            SceneBVH,
            RAY_FLAG_NONE,
            0xFF,
            1,  // Shadow hit group index
            1,
            1,  // Shadow miss shader index
            continuationRay,
            continuationPayload);
        payload.seed = continuationPayload.seed;
        payload.isVisible = continuationPayload.isVisible;
        return;
    }
    // Surface blocks the shadow ray (random draw fell in the opaque fraction).
    payload.isVisible = false;
}

[shader("closesthit")]
void SphereShadowHit(
    inout ShadowPayload payload,
    in BuiltInTriangleIntersectionAttributes attribs)
{
    // For shadow rays hitting spheres, check opacity similar to triangle shadow
    // hit
    payload.isVisible = true;

    // Limit recursion depth
    const uint MAX_SHADOW_OPACITY = 4;
    if (payload.recursionDepth >= MAX_SHADOW_OPACITY) {
        payload.isVisible = false;
        return;
    }

    uint instance_id = InstanceID();

    // Get material ID from instance
    var material_id = instanceDescBuffer[instance_id].materialID;

    if (material_id == uint(-1)) {
        // No material, assume opaque
        payload.isVisible = false;
        return;
    }

    var material_header = materialHeaderBuffer[material_id];

    // Create a dummy vertex for the sphere hit
    VertexInfo vertex;
    vertex.texC = float2(0, 0);
    vertex.subsetMaterialID = -1;

    // Fetch opacity
    FetchCallableData opacityFetchData;
    opacityFetchData.vertexInfo = vertex;
    opacityFetchData.materialBlobID = material_header.material_blob_id;
    opacityFetchData.material_params_index = 0;
    opacityFetchData.shader_type_id = 0;
    opacityFetchData.opacityColor = float3(1.0, 1.0, 1.0);

    CallShader(
        ptConstants.materialFetchCallableBaseIndex +
            material_header.material_type_id +
            ptConstants.materialOpacityCallableOffset,
        opacityFetchData);

    float opacity = asfloat(opacityFetchData.material_params_index);

    // Stochastic opacity test — symmetric with ShadowHit above and the main
    // radiance loop. See ShadowHit for rationale.
    if (random_float(payload.seed) >= opacity) {
        // Surface is transparent to this shadow ray — continue tracing.
        float3 hitPoint =
            WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
        float remainingDist = payload.rayTMax - RayTCurrent();
        if (remainingDist <= 0.001) {
            payload.isVisible = true;
            return;
        }

        RayDesc continuationRay;
        continuationRay.Origin = hitPoint;
        continuationRay.Direction = WorldRayDirection();
        continuationRay.TMin = 0.001;
        continuationRay.TMax = remainingDist;

        ShadowPayload continuationPayload;
        continuationPayload.isVisible = true;
        continuationPayload.rayTMax = remainingDist;
        continuationPayload.recursionDepth = payload.recursionDepth + 1;
        continuationPayload.seed = payload.seed;

        TraceRay(
            SceneBVH,
            RAY_FLAG_NONE,
            0xFF,
            1,
            1,
            1,
            continuationRay,
            continuationPayload);
        payload.seed = continuationPayload.seed;
        payload.isVisible = continuationPayload.isVisible;
        return;
    }
    // Surface blocks the shadow ray.
    payload.isVisible = false;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.isVisible = true;
}

// ============================================================================
// Material Evaluation Callables
// ============================================================================
// Note: Callable implementations are now in separate files:
// - callables/eval_fallback.slang
// - callables/eval_standard_surface.slang
// - callables/eval_preview_surface.slang
// They are linked via add_source_code() in the C++ code.
