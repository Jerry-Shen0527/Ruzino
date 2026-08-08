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

    // Volume hit state (WetbrushVolume density slab). When isVolume is true the
    // path tracer treats this as an emission/absorption event: raymarch the
    // segment inside VolumeClosestHit, accumulate Beer-Lambert-attenuated paint
    // color, and add it to the radiance directly (no surface material).
    bool isVolume;
    float3 volumeColor;
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
            payload.isVolume = false;
            payload.volumeColor = float3(0.0);

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
                    // Hosek analytic sky fields (ignored by non-Hosek callables).
                    domeData.hosekStateIndex = domeLight.hosekStateIndex;
                    domeData.sunDirLocal = domeLight.dirW;

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

            // Volume hit (WetbrushVolume density slab): VolumeClosestHit did
            // the paper §6 emission/absorption raymarch and accumulated the
            // Beer-Lambert-attenuated paint color in payload.volumeColor. Add
            // it to the radiance and terminate the path -- no surface material
            // is consulted for volume hits.
            if (payload.isVolume) {
                accumulated_color = add(
                    accumulated_color,
                    mul(current_throughput, Spectrum(payload.volumeColor)));
                break;
            }

            // Process hit - material evaluation
            VertexInfo vertex = payload.vertex;

            // output_color = Spectrum(vertex.tangentW.xyz * 0.5 + 0.5);
            // output_color = Spectrum(float3(payload.eta_flipped, 0, 0));
            // break;
            uint material_id = payload.materialID;
            uint eta_flipped = payload.eta_flipped;

            // No material assigned — use fallback Lambertian eval (index 2).
            // Mirrors the regular material path below: sample a (non-dome)
            // light, eval the fallback BRDF, accumulate direct lighting, update
            // throughput, then continue bouncing. Dome light is picked up
            // automatically on the next-bounce miss. (Previously this branch
            // ran its own dome-skipping direct-lighting loop and then
            // `break`-terminated, so fallback objects went fully black under
            // dome-only lighting and received no indirect light.)
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
    // presence pass-through. With probability (1-opacity) the shadow ray
    // passes through and keeps tracing, so a semi-transparent surface casts
    // a soft partial shadow. Replaces the dead `opacity_threshold` (which
    // burned a random number but never used it) and the `opacity < 0.25`
    // binary cutoff.
    if (random_float(payload.seed) >= opacity) {
        // Surface is transparent to this shadow ray — continue tracing.
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
    // Surface blocks the shadow ray.
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


// ============================================================================
// Wetbrush volume hit groups (added by wetbrush_render node).
// References VolumeDesc / volumeDescBuffer (declared above via the Scene
// imports + BindlessVertexBuffer) and RayPayload / ShadowPayload (declared
// above). Helpers live in volume_intersection.slang (imported here).
// ============================================================================
import volume_intersection;

// Paper §6 "Liquid rendering" ambient occlusion: "by shooting 64 rays around
// the intersection point and collecting the maximum density values these rays
// can reach within a short distance." Sample the density field directly (no
// BVH) along N short rays distributed over the upper hemisphere around the
// surface normal; rays that walk through dense paint contribute occlusion.
// Returns an occlusion factor in [0,1] where 1 = fully occluded (darkens the
// paint in crevices/dense regions) and 0 = fully exposed (bright).
float ambientOcclusion(VolumeDesc vd, float3 p, float3 n, uint seed)
{
    // Paper §6: "shooting 64 rays around the intersection point".
    const uint N = 64;
    const float aoRadius = vd.cellSize * 10.0;  // "short distance" ~= a few cells
    const int aoSteps = 6;             // samples per AO ray within aoRadius
    const float aoStepLen = aoRadius / float(aoSteps);

    // Build an orthonormal basis around n.
    float3 up = abs(n.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    float occlusion = 0.0;
    for (uint i = 0; i < N; ++i) {
        // Fibonacci spiral on the hemisphere: gives a quasi-uniform spread.
        float phi = sqrt(float(i) + 0.5) * 2.39996323;  // golden angle
        float cosTheta = 1.0 - (float(i) + 0.5) / float(N);
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        float3 rayDir = localDir.x * t + localDir.y * b + localDir.z * n;

        // March the short ray; accumulate the max density it reaches (paper:
        // "maximum density values these rays can reach").
        float maxDensity = 0.0;
        for (int s = 1; s <= aoSteps; ++s) {
            float3 sp = p + rayDir * aoStepLen * float(s);
            float d = samplePaintField(vd, sp).x;
            maxDensity = max(maxDensity, d);
        }
        // Occlusion contribution: a ray walking through dense paint darkens.
        occlusion += min(maxDensity, 1.0);
    }
    return occlusion / float(N);
}

[shader("intersection")]
void VolumeIntersection()
{
    uint instance_id = InstanceID();
    uint vol_id = instanceDescBuffer[instance_id].geometryID;
    VolumeDesc vd = volumeDescBuffer[vol_id];
    float tEnter, tExit;
    if (!intersectSlab(vd, tEnter, tExit))
        return;

    // Paper §6 first-cross, evaluated in the intersection stage: only report
    // a hit if the ray segment actually crosses a paint cell (density above
    // the surface threshold). Empty volume segments must NOT report —
    // otherwise they swallow the ray (one TraceRay = one result) and the
    // paint reads as drawn on the dome background instead of on the paper
    // plane behind the volume. Marching the segment here mirrors what
    // VolumeClosestHit's Phase 1 does; the cost is bounded by the slab
    // depth (5 cells for the test scene).
    float3 origin = WorldRayOrigin();
    float3 dir = WorldRayDirection();
    float step = vd.cellSize * 0.25;  // 1/4 cell, paper §6 cadence
    float segLen = tExit - tEnter;
    int maxSteps = 256;
    int steps = min(maxSteps, int(ceil(segLen / step)));
    float actualStep = segLen / float(max(1, steps));
    for (int i = 0; i < steps; ++i) {
        float t = tEnter + (float(i) + 0.5) * actualStep;
        float3 p = origin + t * dir;
        if (sampleDensity(vd, p) > kDensitySurface) {
            VolumeHitAttributes attr;
            attr.tEnter = tEnter;
            attr.tExit = tExit;
            ReportHit(tEnter, 0u, attr);
            return;
        }
    }
    // No paint along this segment: do not report. Ray continues to the next
    // hit (e.g. the paper plane behind the volume).
}

[shader("closesthit")]
void VolumeClosestHit(inout RayPayload payload, in VolumeHitAttributes attr)
{
    // Paper §6 "Liquid rendering", first-cross + penetration blend:
    //   "we sample the density value every one quarter of the density grid
    //    size ... [find] the first sample along the ray that exceeds the
    //    surface threshold ... To produce transparency effects, we use the oil
    //    density to calculate a penetration distance and we allow more pigment
    //    samples to be collected along the ray into the surface within that
    //    distance. We blend all of the pigment samples together to determine
    //    the final color of the ray."
    //
    // This is NOT a Beer-Lambert extinction integral (that was the previous
    // implementation, which produced near-zero radiance for thin paint layers:
    // density 0.1 × 3 Z-cells accumulates <0.003 extinction → invisible).
    // Instead: march at 1/4-cell steps until the FIRST cell whose density
    // exceeds the surface threshold; that is the "first cross". From there,
    // continue marching a penetration depth proportional to the oil density,
    // averaging the pigment color of every cell touched. The blended pigment
    // IS the radiance (RayGen composites volume hits as pure emission). The
    // surface normal (density gradient) modulates simple Lambertian shading;
    // paper §6 AO darkens crevices.
    payload.hit = true;
    payload.isVolume = true;
    payload.volumeColor = float3(0.0);
    uint instance_id = InstanceID();
    uint vol_id = instanceDescBuffer[instance_id].geometryID;
    VolumeDesc vd = volumeDescBuffer[vol_id];
    float3 origin = WorldRayOrigin();
    float3 dir = WorldRayDirection();
    float cell_sz = vd.cellSize;
    // Paper: sample "every one quarter of the density grid size".
    float step = cell_sz * 0.25;
    float segLen = attr.tExit - attr.tEnter;
    int maxSteps = 256;
    int steps = min(maxSteps, int(ceil(segLen / step)));
    float actualStep = segLen / float(max(1, steps));

    // Phase 1: first-cross. March until the first cell with density above the
    // surface threshold. Record its world position; that is the surface point.
    float tCross = -1.0;
    float densityAtCross = 0.0;
    for (int i = 0; i < steps; ++i) {
        float t = attr.tEnter + (float(i) + 0.5) * actualStep;
        float3 p = origin + t * dir;
        float d = sampleDensity(vd, p);
        if (d > kDensitySurface) {
            tCross = t;
            densityAtCross = d;
            break;
        }
    }
    // Defensive: VolumeIntersection now only reports when the segment crosses
    // a paint cell, so tCross should always be found. Keep the guard so a
    // future regression in the intersection shader can never re-introduce the
    // "opaque black box" bug.
    if (tCross < 0.0) {
        payload.hit = false;
        payload.isVolume = false;
        return;
    }

    // Phase 2: penetration blend. Continue from the first-cross point for a
    // depth proportional to the oil density (denser paint = deeper collect),
    // averaging the pigment color of every cell touched. Paper: "we allow more
    // pigment samples to be collected along the ray into the surface within
    // [the penetration] distance."
    float penDepth = max(cell_sz, densityAtCross * cell_sz * 4.0);
    float tPenEnd = min(tCross + penDepth, attr.tExit);
    float3 colorSum = float3(0.0);
    float weightSum = 0.0;
    for (int j = 0; j < steps; ++j) {
        float t = tCross + (float(j) + 0.5) * actualStep;
        if (t > tPenEnd)
            break;
        float3 p = origin + t * dir;
        float4 cell = samplePaintField(vd, p);
        float d = cell.x;
        if (d <= 0.0)
            continue;
        // Weight each sample by its density: denser pigment dominates the blend.
        float w = d;
        colorSum += w * float3(cell.y, cell.z, cell.w);
        weightSum += w;
    }
    float3 paintColor = (weightSum > 0.0) ? colorSum / weightSum : float3(0.0);

    // Surface normal from the density gradient at the first-cross point.
    float3 surfP = origin + tCross * dir;
    float3 nrm = surfaceNormal(vd, surfP);

    // Simple Lambertian facing term: paint facing the incoming ray direction
    // is brightest; grazing paint darkens. dir points INTO the surface, so the
    // facing term uses -dir (the view direction).
    float facing = max(0.0, dot(nrm, -dir));
    float shade = 0.4 + 0.6 * facing;  // ambient floor + Lambertian

    // Paper §6 ambient occlusion at the surface point: 64-ray hemisphere
    // sampling darkens crevices and dense pigment pockets.
    float ao = ambientOcclusion(vd, surfP, nrm, payload.seed);

    payload.volumeColor = paintColor * shade * (1.0 - ao);
}

[shader("closesthit")]
void VolumeShadowHit(inout ShadowPayload payload, in VolumeHitAttributes attr)
{
    // Paper §6 shadow handling for the paint surface: a shadow ray that first
    // crosses a cell above the surface threshold is blocked (the paint surface
    // occludes the light). This mirrors VolumeClosestHit's first-cross test
    // rather than the previous Beer-Lambert integral, which never reached
    // sufficient extinction to block light for thin paint layers.
    uint instance_id = InstanceID();
    uint vol_id = instanceDescBuffer[instance_id].geometryID;
    VolumeDesc vd = volumeDescBuffer[vol_id];
    float3 origin = WorldRayOrigin();
    float3 dir = WorldRayDirection();
    float cell_sz = vd.cellSize;
    float step = cell_sz * 0.25;  // 1/4 cell, same cadence as the closest hit
    float segLen = attr.tExit - attr.tEnter;
    int maxSteps = 256;
    int steps = min(maxSteps, int(ceil(segLen / step)));
    float actualStep = segLen / float(max(1, steps));
    for (int i = 0; i < steps; ++i) {
        float t = attr.tEnter + (float(i) + 0.5) * actualStep;
        float3 p = origin + t * dir;
        float d = sampleDensity(vd, p);
        if (d > kDensitySurface) {
            payload.isVisible = false;
            return;
        }
    }
}
