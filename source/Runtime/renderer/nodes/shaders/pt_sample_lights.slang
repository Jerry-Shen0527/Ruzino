import pt_constants;
__exported import Scene.Lights.LightData;
__exported import Scene.Lights.HosekSkyState;
__exported import Scene.Lights.LightCollectionShared;
import utils.Math.PackedFormats;
StructuredBuffer<LightData> lightBuffer;

// PackedNode: 32B = 2× uint4. We define it locally (not via
// import Scene.Lights.LightBVHTypes) because importing the full module
// — whose PackedNode methods call f16tof32/decodeNormal2x16 — triggers
// Slang::InternalError "unknown stage" when compiled into the multi-stage
// RT pipeline (raygen+closesthit+miss+callable). Instead we inline the
// attribute decoding here as free functions, reading the raw data[2] dwords
// directly. This avoids the cross-module method-resolution that breaks.
struct PackedNode { uint4 data[2]; };

// --- BVH node attribute decoding (inlined, matches LightBVHTypes.slang
// bit layout exactly: see packInternalNode/setNodeAttributes) ---

// MSB of data[0].x: 1=leaf, 0=internal.
bool bvhIsLeaf(PackedNode n) { return (n.data[0].x >> 31) != 0; }

// Leaf: triangleCount (top 4 bits below MSB) + triangleOffset (remaining 27).
uint bvhLeafTriangleCount(PackedNode n) {
    return (n.data[0].x >> 27) & 0xFu;
}
uint bvhLeafTriangleOffset(PackedNode n) {
    return n.data[0].x & 0x07FFFFFFu;
}
// Internal: rightChildIdx is the full data[0].x (MSB=0, so value is direct).
uint bvhInternalRightChild(PackedNode n) { return n.data[0].x; }

// Shared attributes: origin = data[0].yzw (3× f32).
float3 bvhNodeOrigin(PackedNode n) {
    return float3(asfloat(n.data[0].y), asfloat(n.data[0].z), asfloat(n.data[0].w));
}
// Extent = f16×3 packed into data[1].xy low halves.
float3 bvhNodeExtent(PackedNode n) {
    uint ex = n.data[1].x & 0xFFFFu;
    uint ey = n.data[1].x >> 16;
    uint ez = n.data[1].y & 0xFFFFu;
    return float3(f16tof32(ex), f16tof32(ey), f16tof32(ez));
}
// Flux = data[1].w (f32).
float bvhNodeFlux(PackedNode n) { return asfloat(n.data[1].w); }
// Cone direction = octahedral 2×16 snorm in data[1].z.
float3 bvhNodeConeDir(PackedNode n) { return decodeNormal2x16(n.data[1].z); }
// cosConeAngle = data[1].y high 16 bits, quantized to [-1,1].
float bvhNodeCosConeAngle(PackedNode n) {
    uint packed = n.data[1].y >> 16;
    return float(packed) * (1.0 / 32767.0) - 1.0;
}

// Emissive mesh light buffers (bound by path_tracing.cpp).
StructuredBuffer<PackedEmissiveTriangle> emissiveTriangleData;
StructuredBuffer<EmissiveFlux> emissiveFluxData;
StructuredBuffer<PackedNode> bvhNodes;
StructuredBuffer<uint> bvhTriangleIndices;
// Per-triangle 64-bit traversal bitmask (for evalPdf reverse traversal).
// bit d set => right child at depth d. Indexed by global triangle index.
StructuredBuffer<uint2> bvhTriangleBitmasks;
// Per-geometry-instance offset into emissiveTriangleData (kInvalidIndex if the
// instance has no emissive triangles). Used by getEmissiveTriangleIndex for the
// BSDF-hit-emissive reverse lookup in ClosestHit.
StructuredBuffer<uint> emissivePerMeshInstanceOffset;

// Invalid-offset / invalid-triangle sentinel (matches emissive_light_globals).
static const uint kInvalidEmissiveIndex = 0xffffffff;

// Map (instanceID, primitiveIndex) -> global emissive triangle index, or
// kInvalidEmissiveIndex if the hit instance/primitive is not emissive. This is
// the BSDF-hit-emissive reverse lookup (mirrors emissive_light_globals' helper
// but declared here so it is visible to path_tracing.slang via the module
// import). triangleCount is ptConstants.emissiveTriangleCount.
uint getEmissiveTriangleIndex(
    uint instanceID, uint primitiveIndex, uint triangleCount)
{
    if (triangleCount == 0) return kInvalidEmissiveIndex;
    uint offset = emissivePerMeshInstanceOffset[instanceID];
    return offset != kInvalidEmissiveIndex ? offset + primitiveIndex
                                            : kInvalidEmissiveIndex;
}

// Per-Hosek-dome-light cooked polynomial state (CPU-cooked from turbidity,
// albedo, solar elevation). Indexed by LightData.hosekStateIndex. Bound by
// path_tracing.cpp / wetbrush_render.cpp alongside lightBuffer. Stays empty
// (one dummy row) in scenes with no Hosek sky.
StructuredBuffer<HosekSkyState> hosekStateBuffer;

#include "utils/random.slangh"

struct LightSample {
    float3 direction;  // Normalized direction to light
    float3 intensity;  // Light intensity
    float pdf;         // PDF of selecting this light and sampling point
    bool valid;        // Whether the sample is valid
    // True for delta lights (Point/Distant/Directional) that have no finite
    // geometry and thus can NEVER be hit by a BSDF-sampled ray. For these the
    // MIS weight must be 1 (no BSDF-hit estimator exists to share with),
    // otherwise the NEE contribution is wrongly attenuated.
    bool delta;
};

struct ShadowPayload {
    float3 hitPos;        // Hit position for continuation rays
    float3 rayDir;        // Ray direction for continuation
    float rayTMax;        // Maximum ray distance
    uint recursionDepth;  // Current recursion depth for opacity testing
    uint seed;
    bool isVisible;  // Whether light is visible or occluded
    // Volumetric Beer-Lambert transmittance accumulated by a cloud shadow
    // hit (CloudShadowHit). Defaults to 1.0 (fully transparent) for surface
    // shadow rays, so existing surface lighting is unaffected; cloud segments
    // multiply it down. Read by the NEE path to attenuate direct light.
    float transmittance = 1.0;
};

// Cosine-weighted hemisphere sampling around normal
float3 cosineSampleHemisphere(float xi1, float xi2, float3 normal)
{
    float r = sqrt(xi1);
    float phi = 2.0 * 3.14159265359 * xi2;
    float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - xi1)));

    float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return localDir.x * tangent + localDir.y * bitangent + localDir.z * normal;
}

// ============================================================================
// Emissive mesh NEE sampling via LightBVH importance traversal.
//
// Algorithm (Conty-Kulla 2018): top-down descent where, at each internal node,
// we pick the left or right child with probability proportional to each
// child's *importance* relative to the shading point. Importance combines the
// node's total flux with a conservative solid-angle/cosine bound derived from
// the node AABB. At a leaf, we select one triangle (uniformly by area for now)
// and sample a point on it.
//
// LIMITATION: orientation cone culling (decodeNormal2x16) is omitted because
// calling it inside computeNodeImportance() — which runs twice per loop
// iteration in the [loop] while-descent below, in the closesthit shader —
// triggers Slang::InternalError "unknown stage". The AABB cosine bound
// provides the primary spatial importance; only back-facing-node early cull
// is missing. See computeNodeImportance() NOTE below.
// ============================================================================

/// Conservative importance of a BVH node relative to a shading point.
/// Simplified from Falcor's computeImportance: flux * |cosBound| / dist^2,
/// where dist is clamped to the node's AABB half-extent to avoid singularity
/// when the shading point is very close to the node. The cosine bound comes
/// from the AABB subtended-cone: the node lies within angle thetaCone around
/// the direction to the node center, so the worst-case NdotL is at least
/// cos(thetaL + thetaCone) where thetaL is the angle between the shading
/// normal and the direction to the node center.
float computeNodeImportance(
    float3 shadingPos, float3 shadingNormal, uint nodeIndex)
{
    PackedNode n = bvhNodes[nodeIndex];
    float flux = bvhNodeFlux(n);
    if (flux <= 0.0) return 0.0;

    float3 center = bvhNodeOrigin(n);
    float3 extent = bvhNodeExtent(n);
    float3 toCenter = center - shadingPos;
    float dist = length(toCenter);
    float halfRadius = length(extent);
    dist = max(dist, halfRadius);
    if (dist < 1e-6) return 0.0;

    float3 dirToNode = toCenter / max(length(toCenter), 1e-6);

    // Conservative cosine bound from AABB subtended cone.
    float sinThetaCone = min(halfRadius / dist, 1.0);
    float cosThetaCone = sqrt(max(0.0, 1.0 - sinThetaCone * sinThetaCone));
    float cosThetaL = dot(shadingNormal, dirToNode);
    float sinThetaL = sqrt(max(0.0, 1.0 - cosThetaL * cosThetaL));
    float cosBound = cosThetaL * cosThetaCone - sinThetaL * sinThetaCone;
    if (cosBound <= 0.0) return 0.0;

    // NOTE: orientation cone culling (bvhNodeConeDir/bvhNodeCosConeAngle) is
    // omitted because calling decodeNormal2x16 inside this function, which is
    // called twice per loop iteration inside sampleLightBVH's [loop] while in
    // the closesthit shader, triggers Slang::InternalError "unknown stage".
    // The AABB cosine bound above already provides the primary spatial
    // importance; the cone would additionally cull back-facing nodes.

    return flux * cosBound / (dist * dist);
}

/// Traverse the LightBVH top-down, selecting one leaf via importance sampling.
/// Returns the selected leaf's triangle index and accumulates the traversal
/// pdf (product of branch-selection probabilities). Returns false if no valid
/// leaf was found.
bool sampleLightBVH(
    float3 hitPos, float3 hitNormal, inout uint seed,
    out uint triangleIndex, out float traversalPdf)
{
    traversalPdf = 1.0;
    triangleIndex = 0xffffffffu;

    uint nodeIndex = 0;
    uint depth = 0;
    const uint kMaxDepth = 64;

    [loop]
    while (depth < kMaxDepth) {
        PackedNode node = bvhNodes[nodeIndex];

        if (bvhIsLeaf(node)) {
            uint triCount = bvhLeafTriangleCount(node);
            if (triCount == 0) return false;
            float r = random_float(seed);
            uint localIdx = uint(r * float(triCount));
            if (localIdx >= triCount) localIdx = triCount - 1;
            triangleIndex =
                bvhTriangleIndices[bvhLeafTriangleOffset(node) + localIdx];
            traversalPdf *= 1.0 / float(triCount);
            return true;
        }

        // DIAGNOSTIC: call computeNodeImportance (currently returns flux only)
        uint leftChildIdx = nodeIndex + 1;
        uint rightChildIdx = bvhInternalRightChild(node);

        float leftImp = computeNodeImportance(hitPos, hitNormal, leftChildIdx);
        float rightImp = computeNodeImportance(hitPos, hitNormal, rightChildIdx);
        float totalImp = leftImp + rightImp;
        if (totalImp <= 0.0) return false;

        float leftProb = leftImp / totalImp;
        if (random_float(seed) < leftProb) {
            traversalPdf *= leftProb;
            nodeIndex = leftChildIdx;
        } else {
            traversalPdf *= (1.0 - leftProb);
            nodeIndex = rightChildIdx;
        }
        depth++;
    }

    return false;
}

/// Sample an emissive light via BVH importance traversal. Falls back to uniform
/// selection if the BVH is empty or traversal fails.
LightSample sampleEmissiveLight(float3 hitPos, float3 hitNormal, inout uint seed)
{
    LightSample s;
    s.direction = float3(0.0);
    s.intensity = float3(0.0);
    s.pdf = 0.0;
    s.valid = false;
    s.delta = false;

    uint emissiveCount = ptConstants.emissiveTriangleCount;
    if (emissiveCount == 0) return s;

    uint triIdx;
    float traversalPdf;
    bool bvhOk = sampleLightBVH(hitPos, hitNormal, seed, triIdx, traversalPdf);

    // Fallback to uniform selection if BVH traversal fails (e.g. all nodes
    // have zero importance from this shading point, or BVH is empty).
    if (!bvhOk) {
        float r = random_float(seed);
        triIdx = uint(r * float(emissiveCount));
        if (triIdx >= emissiveCount) triIdx = emissiveCount - 1;
        traversalPdf = 1.0 / float(emissiveCount);
    }

    // Read triangle data directly from packed fields.
    PackedEmissiveTriangle packedTri = emissiveTriangleData[triIdx];
    float3 v0 = packedTri.posAndTexCoords[0].xyz;
    float3 v1 = packedTri.posAndTexCoords[1].xyz;
    float3 v2 = packedTri.posAndTexCoords[2].xyz;
    float area = asfloat(packedTri.area);

    // Compute geometric normal from vertices.
    float3 normal = normalize(cross(v1 - v0, v2 - v0));

    // Sample a point on the triangle (uniform area).
    float u1 = random_float(seed);
    float u2 = random_float(seed);
    if (u1 + u2 > 1.0) { u1 = 1.0 - u1; u2 = 1.0 - u2; }
    float3 samplePos = v0 * (1.0 - u1 - u2) + v1 * u1 + v2 * u2;

    float3 dir = samplePos - hitPos;
    float dist = length(dir);
    if (dist < 1e-6) return s;
    dir = dir / dist;

    float LdotN = max(0.0, dot(-dir, normal));
    if (LdotN < 1e-6) return s;
    if (area < 1e-8) return s;

    // Solid-angle pdf = traversalPdf * (1/area) * dist^2 / LdotN.
    // The traversalPdf already encodes the BVH branch + leaf triangle
    // selection probability; the remaining term converts area sampling to
    // solid-angle measure (matching the Rect/Disc convention in sampleLights).
    float areaPdf = 1.0 / area;
    float solidAnglePdf = traversalPdf * areaPdf * dist * dist / LdotN;

    s.direction = dir;
    s.intensity = emissiveFluxData[triIdx].averageRadiance;
    s.pdf = solidAnglePdf;
    s.valid = true;
    s.delta = false;
    return s;
}

// Evaluate the emissive-mesh NEE pdf for a specific emissive triangle index,
// given the shading point and the world-space hit geometry. This is the
// reverse of sampleLightBVH / sampleEmissiveLight: instead of drawing a
// random traversal, we re-walk the EXACT path to `triangleIndex` (encoded in
// bvhTriangleBitmasks[triangleIndex]) and accumulate the branch-selection
// probabilities we would have used, using the same computeNodeImportance as
// the forward sampler. The result is the solid-angle pdf of this direction
// under the emissive NEE distribution, directly comparable to bsdf_pdf for
// the BSDF-hit-emissive MIS weight w_bsdf = bsdf_pdf / (emis_pdf + bsdf_pdf).
//
// Returns 0 if the triangle is not reachable (e.g. zero-importance path,
// which would have triggered the uniform fallback in the forward sampler —
// we do NOT replicate the fallback here; a zero traversalPdf honestly means
// the NEE estimator would almost never sample this direction, so the BSDF
// estimator should take near-full weight).
//
// `hitPos`      — shading point (ray origin of the BSDF sample).
// `hitNormal`   — shading normal at hitPos.
// `triangleIndex` — global emissive triangle index that was actually hit.
// `dir`         — normalized world-space direction from hitPos to the
//                 emissive surface (the BSDF-sampled direction).
// `dist`        — distance from hitPos to the emissive surface.
float evalEmissivePdf(
    float3 hitPos, float3 hitNormal, uint triangleIndex,
    float3 dir, float dist)
{
    uint emissiveCount = ptConstants.emissiveTriangleCount;
    if (emissiveCount == 0) return 0.0;
    if (triangleIndex >= emissiveCount) return 0.0;

    // Read the traversal path: bit d of the 64-bit mask tells which child was
    // taken at depth d (0=left nodeIndex+1, 1=right bvhInternalRightChild).
    uint2 mask = bvhTriangleBitmasks[triangleIndex];
    uint64_t bitmask =
        uint64_t(mask.x) | (uint64_t(mask.y) << 32);

    float traversalPdf = 1.0;
    uint nodeIndex = 0;
    uint depth = 0;
    const uint kMaxDepth = 64;

    [loop]
    while (depth < kMaxDepth) {
        PackedNode node = bvhNodes[nodeIndex];

        if (bvhIsLeaf(node)) {
            uint triCount = bvhLeafTriangleCount(node);
            if (triCount == 0) return 0.0;
            // Uniform triangle selection within the leaf — same as the
            // forward sampler. The target is assumed present (the bitmask
            // led us here), so this factor applies regardless of which
            // triangle in the leaf was hit.
            traversalPdf *= 1.0 / float(triCount);
            break;
        }

        uint leftChildIdx = nodeIndex + 1;
        uint rightChildIdx = bvhInternalRightChild(node);

        // Branch probabilities (identical math to sampleLightBVH).
        float leftImp = computeNodeImportance(hitPos, hitNormal, leftChildIdx);
        float rightImp = computeNodeImportance(hitPos, hitNormal, rightChildIdx);
        float totalImp = leftImp + rightImp;
        if (totalImp <= 0.0) return 0.0;  // unreachable via importance

        float leftProb = leftImp / totalImp;
        // Which child does the path take at this depth?
        bool goRight = ((bitmask >> depth) & 1ull) != 0ull;
        if (goRight) {
            traversalPdf *= (1.0 - leftProb);
            nodeIndex = rightChildIdx;
        } else {
            traversalPdf *= leftProb;
            nodeIndex = leftChildIdx;
        }
        depth++;
    }

    // Solid-angle conversion — identical to sampleEmissiveLight: the forward
    // sampler picks a point uniformly by area, then we convert that area pdf
    // to solid-angle measure via dist^2 / LdotN. LdotN is the cosine between
    // the emissive surface normal and -dir (the light's outward direction
    // dotted with the direction toward the shading point). We recompute it
    // from the triangle geometry so this stays consistent even when the hit
    // surface normal differs slightly (e.g. due to interpolation / face flip).
    PackedEmissiveTriangle packedTri = emissiveTriangleData[triangleIndex];
    float3 v0 = packedTri.posAndTexCoords[0].xyz;
    float3 v1 = packedTri.posAndTexCoords[1].xyz;
    float3 v2 = packedTri.posAndTexCoords[2].xyz;
    float area = asfloat(packedTri.area);
    if (area < 1e-8) return 0.0;

    float3 triNormal = normalize(cross(v1 - v0, v2 - v0));
    float LdotN = max(0.0, dot(-dir, triNormal));
    if (LdotN < 1e-6) return 0.0;
    if (dist < 1e-6) return 0.0;

    float areaPdf = 1.0 / area;
    float solidAnglePdf = traversalPdf * areaPdf * dist * dist / LdotN;

    // When analytic lights also exist, the forward sampler flips a 50/50
    // coin before choosing emissive, scaling the emissive pdf by 0.5. Mirror
    // that here so this pdf is on the same footing as the analytic pdfs in
    // lightPdfForDir (which would likewise be halved). When emissive is the
    // only estimator, no scaling.
    bool hasAnalytic = ptConstants.lightCount > 0;
    if (hasAnalytic) solidAnglePdf *= 0.5;

    return solidAnglePdf;
}

// Sample a random light and return its direction, intensity, and PDF
LightSample sampleLights(float3 hitPos, float3 hitNormal, inout uint seed)
{
    LightSample sample;
    sample.valid = false;

    bool hasAnalytic = ptConstants.lightCount > 0;
    bool hasEmissive = ptConstants.emissiveTriangleCount > 0;
    if (!hasAnalytic && !hasEmissive) return sample;

    // When both exist, flip a coin (50/50). When only one exists, use it.
    if (hasAnalytic && hasEmissive) {
        if (random_float(seed) >= 0.5) {
            sample = sampleEmissiveLight(hitPos, hitNormal, seed);
            if (sample.valid) { sample.pdf *= 0.5; return sample; }
        }
    } else if (hasEmissive) {
        return sampleEmissiveLight(hitPos, hitNormal, seed);
    }

    // Analytic light path (existing code below).
    if (ptConstants.lightCount == 0)
        return sample;

    // Count non-dome lights (dome lights are handled by miss shader)
    uint sampleableLightCount = 0;
    for (uint i = 0; i < ptConstants.lightCount; i++) {
        if (lightBuffer[i].type != uint(LightType::Dome)) {
            sampleableLightCount++;
        }
    }

    if (sampleableLightCount == 0)
        return sample;

    // Randomly select a non-dome light
    uint targetLight = uint(random_float(seed) * sampleableLightCount);
    uint lightIndex = 0;
    uint nonDomeCount = 0;
    for (uint i = 0; i < ptConstants.lightCount; i++) {
        if (lightBuffer[i].type != uint(LightType::Dome)) {
            if (nonDomeCount == targetLight) {
                lightIndex = i;
                break;
            }
            nonDomeCount++;
        }
    }

    LightData light = lightBuffer[lightIndex];
    float lightPdf =
        1.0 / float(sampleableLightCount);  // PDF for light selection
    float3 lightDir;
    float lightDistance;
    float areaPdf = 1.0;

    // Sample the selected light based on its type
    if (light.type == uint(LightType::Point)) {
        lightDir = light.posW - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        if (NdotL > 0.0) {
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + hitNormal * 0.0001;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.0;
            shadowRay.TMax = lightDistance - 0.0001;

            ShadowPayload shadowPayload;
            shadowPayload.isVisible = true;
            shadowPayload.rayTMax = lightDistance - 0.0001;
            shadowPayload.recursionDepth = 0;
            shadowPayload.seed = seed;
            TraceRay(
                SceneBVH,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
                0xFD,  // exclude light instances (mask 0x2) -- lights don't occlude
                1,
                0,
                1,
                shadowRay,
                shadowPayload);
            seed = shadowPayload.seed;
            if (shadowPayload.isVisible) {
                float attenuation = 1.0 / (lightDistance * lightDistance);
                sample.direction = lightDir;
                sample.intensity = light.intensity * attenuation;
                sample.pdf = lightPdf;
                sample.valid = true;
                sample.delta = true;  // point light: no BSDF-hit estimator
            }
        }
    }
    else if (light.type == uint(LightType::Sphere)) {
        // Sphere light sampling using uniform cone sampling.
        // For a diffuse (Lambertian) sphere emitter, radiance L = intensity / pi.
        // The path tracer computes: L * NdotL * bsdf / pdf.
        float3 wc = light.posW - hitPos;
        float dist = length(wc);
        float3 wcN = wc / dist;

        float radius = sqrt(light.surfaceArea / (4.0 * 3.14159265359));
        float3 sphereRadiance = light.intensity / 3.14159265359;

        if (dist <= radius) {
            float3 dir = cosineSampleHemisphere(
                random_float(seed), random_float(seed), hitNormal);
            sample.direction = dir;
            sample.intensity = sphereRadiance;
            sample.pdf = lightPdf;
            sample.valid = true;
            sample.delta = false;
        }
        else {
            float sinThetaMax = min(radius / dist, 1.0);
            float cosThetaMax = sqrt(1.0 - sinThetaMax * sinThetaMax);

            float xi1 = random_float(seed);
            float xi2 = random_float(seed);

            float cosTheta = (1.0 - xi1) + xi1 * cosThetaMax;
            float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
            float phi = 2.0 * 3.14159265359 * xi2;

            float3 wcU, wcV;
            if (abs(wcN.x) > 0.9) {
                wcU = normalize(cross(float3(0, 1, 0), wcN));
            }
            else {
                wcU = normalize(cross(float3(1, 0, 0), wcN));
            }
            wcV = cross(wcN, wcU);

            lightDir =
                sinTheta * cos(phi) * wcU +
                sinTheta * sin(phi) * wcV +
                cosTheta * wcN;

            float NdotL = max(0.0, dot(hitNormal, lightDir));
            if (NdotL > 0.0) {
                float3 oc = hitPos - light.posW;
                float b = dot(oc, lightDir);
                float c = dot(oc, oc) - radius * radius;
                float disc = b * b - c;
                lightDistance = -b - sqrt(max(0.0, disc));

                float conePdf =
                    1.0 / (2.0 * 3.14159265359 * (1.0 - cosThetaMax));

                RayDesc shadowRay;
                shadowRay.Origin = hitPos + hitNormal * 0.0001;
                shadowRay.Direction = lightDir;
                shadowRay.TMin = 0.0;
                shadowRay.TMax = max(0.001, lightDistance - 0.001);

                ShadowPayload shadowPayload;
                shadowPayload.isVisible = true;
                shadowPayload.rayTMax = shadowRay.TMax;
                shadowPayload.recursionDepth = 0;
                shadowPayload.seed = seed;

                TraceRay(
                    SceneBVH,
                    RAY_FLAG_NONE,
                    0xFD,  // exclude light instances (mask 0x2) -- lights don't occlude
                    1,
                    1,
                    1,
                    shadowRay,
                    shadowPayload);

                seed = shadowPayload.seed;

                if (shadowPayload.isVisible) {
                    sample.direction = lightDir;
                    sample.intensity = sphereRadiance;
                    sample.pdf = lightPdf * conePdf;
                    sample.valid = true;
                    sample.delta = false;
                }
            }
        }
    }
    else if (light.type == uint(LightType::Distant)) {
        lightDir = -light.dirW;
        lightDistance = 1e4;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        if (NdotL > 0.0) {
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + hitNormal * 0.001;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.001;
            shadowRay.TMax = lightDistance;

            ShadowPayload shadowPayload;
            shadowPayload.isVisible = true;
            shadowPayload.transmittance = 1.0;  // cloud-free until CloudShadowHit multiplies it down
            shadowPayload.rayTMax = lightDistance;
            shadowPayload.recursionDepth = 0;
            shadowPayload.seed = seed;
            TraceRay(
                SceneBVH,
                RAY_FLAG_NONE,
                0xFD,  // exclude light instances (mask 0x2) -- lights don't occlude
                1,
                1,
                1,
                shadowRay,
                shadowPayload);
            seed = shadowPayload.seed;

            if (shadowPayload.isVisible) {
                sample.direction = lightDir;
                // CloudShadowHit attenuates shadowPayload.transmittance by
                // Beer-Lambert through any cloud segment the shadow ray
                // crossed (defaults to 1.0 = no cloud, so cloud-free hits are
                // unaffected). Applying it here turns the previously-binary
                // cloud shadow into a soft one.
                sample.intensity = light.intensity * shadowPayload.transmittance;
                sample.pdf = lightPdf;
                sample.valid = true;
                sample.delta = true;  // distant light: no BSDF-hit estimator
            }
        }
    }
    else if (light.type == uint(LightType::Rect)) {
        float u = random_float(seed);
        float v = random_float(seed);

        float3 localPoint = float3((u - 0.5), (v - 0.5), 0.0);
        float3 lightSamplePos = light.posW + localPoint.x * light.tangent +
                                localPoint.y * light.bitangent;

        lightDir = lightSamplePos - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        float LdotN = max(0.0, dot(-lightDir, light.dirW));

        if (LdotN > 0.0) {
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + hitNormal * 0.0001;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.0;
            shadowRay.TMax = lightDistance - 0.0001;

            ShadowPayload shadowPayload;
            shadowPayload.isVisible = true;
            shadowPayload.rayTMax = lightDistance - 0.0001;
            shadowPayload.recursionDepth = 0;
            shadowPayload.seed = seed;
            TraceRay(
                SceneBVH,
                RAY_FLAG_NONE,
                0xFD,  // exclude light instances (mask 0x2) -- lights don't occlude
                1,
                1,
                1,
                shadowRay,
                shadowPayload);
            seed = shadowPayload.seed;

            if (shadowPayload.isVisible) {
                // Solid-angle measure: intensity is the raw emitted radiance
                // Le, and pdf is the solid-angle pdf p_A * dist^2 / LdotN
                // (converting the area sampling to solid angle). NEE estimator
                // then contributes Le * NdotL * f / pdf_solid — exactly one
                // NdotL, as the rendering equation requires. This also makes
                // the MIS weight w_light = pdf_solid/(pdf_solid + bsdf_pdf)
                // dimensionally consistent with the BSDF-hit estimator (both
                // solid-angle). The old area-form put NdotL into intensity AND
                // the main loop multiplied NdotL again (NdotL^2 bug).
                areaPdf = 1.0 / light.surfaceArea;
                float solidAnglePdf =
                    lightPdf * areaPdf * (lightDistance * lightDistance) / LdotN;
                sample.direction = lightDir;
                sample.intensity = light.intensity;
                sample.pdf = solidAnglePdf;
                sample.valid = true;
                sample.delta = false;
            }
        }
    }
    else if (light.type == uint(LightType::Disc)) {
        float r = sqrt(random_float(seed));
        float theta = 2.0 * 3.14159265359 * random_float(seed);

        float2 localPoint = float2(r * cos(theta), r * sin(theta));
        float3 lightSamplePos = light.posW + localPoint.x * light.tangent +
                                localPoint.y * light.bitangent;

        lightDir = lightSamplePos - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        float LdotN = max(0.0, dot(-lightDir, light.dirW));

        if (NdotL > 0.0 && LdotN > 0.0) {
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + hitNormal * 0.0001;
            shadowRay.Direction = lightDir;
            shadowRay.TMin = 0.0;
            shadowRay.TMax = max(0.0, lightDistance - 0.0001);

            ShadowPayload shadowPayload;
            shadowPayload.isVisible = true;
            shadowPayload.rayTMax = max(0.0, lightDistance - 0.0001);
            shadowPayload.recursionDepth = 0;
            shadowPayload.seed = seed;
            TraceRay(
                SceneBVH,
                RAY_FLAG_NONE,
                0xFD,  // exclude light instances (mask 0x2) -- lights don't occlude
                1,
                1,
                1,
                shadowRay,
                shadowPayload);

            seed = shadowPayload.seed;

            if (shadowPayload.isVisible) {
                // Solid-angle measure (see Rect branch for the rationale).
                areaPdf = 1.0 / light.surfaceArea;
                float solidAnglePdf =
                    lightPdf * areaPdf * (lightDistance * lightDistance) / LdotN;
                sample.direction = lightDir;
                sample.intensity = light.intensity;
                sample.pdf = solidAnglePdf;
                sample.valid = true;
                sample.delta = false;
            }
        }
    }

    // If both analytic and emissive exist and we took the analytic path
    // (50% selection probability), scale pdf accordingly.
    if (sample.valid && hasAnalytic && hasEmissive) {
        sample.pdf *= 0.5;
    }

    return sample;
}

// Evaluate the light-sampling pdf for an arbitrary direction `dir` (the
// BSDF-sampled direction on the BSDF-hit-light MIS path). Returns the SAME pdf
// convention as sampleLights: for area lights (Rect/Disc) it is
// selectionPdf * (1/surfaceArea) [area pdf, with the 1/dist^2 geometric term
// living in intensity, NOT in pdf]; for Sphere it is selectionPdf * conePdf;
// for Point/Distant/Dome (delta / no geometry) it is 0, because a BSDF-sampled
// ray can never hit them. This is the cross-term p_light(L_bsdf) needed for the
// BSDF-hit-light MIS weight: w_bsdf = bsdf_pdf / (light_pdf + bsdf_pdf).
//
// `dir` is the world-space ray direction (normalized by caller).
float lightPdfForDir(float3 hitPos, float3 dir)
{
    if (ptConstants.lightCount == 0) return 0.0;

    // Count non-dome lights to match sampleLights' selection probability.
    uint sampleableLightCount = 0;
    for (uint i = 0; i < ptConstants.lightCount; i++) {
        if (lightBuffer[i].type != uint(LightType::Dome))
            sampleableLightCount++;
    }
    if (sampleableLightCount == 0) return 0.0;

    float selectionPdf = 1.0 / float(sampleableLightCount);
    float totalPdf = 0.0;

    for (uint j = 0; j < ptConstants.lightCount; j++) {
        LightData light = lightBuffer[j];
        if (light.type == uint(LightType::Dome)) continue;

        if (light.type == uint(LightType::Rect) ||
            light.type == uint(LightType::Disc)) {
            // Ray-plane intersection: plane through light.posW with normal
            // light.dirW (emission direction). The quad/disc lies in that
            // plane, spanned by light.tangent / light.bitangent (already
            // scaled by width/height or radius).
            //
            // NOTE: no `abs(denom) < eps` early-skip here. A ray nearly
            // parallel to the light plane yields a large-but-finite t; the
            // u/v in-quad test below naturally rejects it (the hit point is
            // far outside the quad). Skipping on epsilon made lightPdfForDir
            // return 0 for grazing directions that the BLAS still reports as
            // hits, so the BSDF-hit MIS weight degenerated to 1 and produced
            // fireflies.
            float denom = dot(light.dirW, dir);
            if (denom == 0.0) continue;
            float t = dot(light.posW - hitPos, light.dirW) / denom;
            if (t <= 0.0) continue;
            float3 p = hitPos + t * dir;
            float3 d = p - light.posW;
            // Project onto the tangent/bitangent basis (they are orthogonal &
            // scaled by extent, so local coords in [-0.5, 0.5] for Rect, inside
            // unit circle for Disc).
            float u = dot(d, light.tangent) /
                      dot(light.tangent, light.tangent);
            float v = dot(d, light.bitangent) /
                      dot(light.bitangent, light.bitangent);
            bool hit = false;
            if (light.type == uint(LightType::Rect)) {
                hit = (abs(u) <= 0.5 && abs(v) <= 0.5);
            }
            else {  // Disc
                hit = (u * u + v * v <= 0.25);
            }
            if (hit) {
                // Solid-angle pdf — identical measure to sampleLights'
                // Rect/Disk solid-angle pdf and to the BSDF pdf, so both MIS
                // weights (NEE and BSDF-hit) are dimensionally consistent.
                float LdotN = max(0.0, dot(-dir, light.dirW));
                if (LdotN > 1e-6) {
                    float areaPdf = 1.0 / light.surfaceArea;
                    totalPdf +=
                        selectionPdf * areaPdf * (t * t) / LdotN;
                }
            }
        }
        else if (light.type == uint(LightType::Sphere)) {
            // Ray-sphere intersection test. If the ray hits the sphere, the
            // pdf is the cone pdf from sampleLights.
            float3 wc = light.posW - hitPos;
            float dist = length(wc);
            float radius = sqrt(light.surfaceArea / (4.0 * 3.14159265359));
            if (dist <= radius) continue;  // inside: degenerate, skip
            float3 wcN = wc / dist;
            float cosTheta = dot(dir, wcN);
            float sinThetaMax = min(radius / dist, 1.0);
            float cosThetaMax = sqrt(1.0 - sinThetaMax * sinThetaMax);
            if (cosTheta >= cosThetaMax) {
                // Confirm the ray actually reaches the sphere (not just inside
                // its cone but past it): solve ray-sphere for a real positive t.
                float3 oc = hitPos - light.posW;
                float b = dot(oc, dir);
                float c = dot(oc, oc) - radius * radius;
                float disc = b * b - c;
                if (disc >= 0.0) {
                    float t0 = -b - sqrt(disc);
                    if (t0 > 0.0) {
                        float conePdf =
                            1.0 / (2.0 * 3.14159265359 * (1.0 - cosThetaMax));
                        totalPdf += selectionPdf * conePdf;
                    }
                }
            }
        }
        // Point / Distant / Directional: delta distributions or no geometry.
        // A BSDF-sampled ray has measure-zero chance of hitting them, so their
        // pdf contribution for the BSDF direction is 0.
    }

    // When emissive mesh lights also exist, the forward sampler flips a 50/50
    // coin before choosing the analytic path (sampleLights), so the analytic
    // selection probability is halved. Mirror that here so this cross-term
    // pdf is on the same footing as evalEmissivePdf (which also halves when
    // analytic lights coexist). Without this, the analytic pdf would be ~2x
    // too large and wrongly suppress the BSDF-hit estimator's weight.
    bool hasEmissive = ptConstants.emissiveTriangleCount > 0;
    if (hasEmissive) totalPdf *= 0.5;

    return totalPdf;
}
