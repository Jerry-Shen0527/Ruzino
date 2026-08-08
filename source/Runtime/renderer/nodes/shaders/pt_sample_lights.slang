import pt_constants;
__exported import Scene.Lights.LightData;
StructuredBuffer<LightData> lightBuffer;

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

// Sample a random light and return its direction, intensity, and PDF
LightSample sampleLights(float3 hitPos, float3 hitNormal, inout uint seed)
{
    LightSample sample;
    sample.valid = false;

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
                sample.intensity = light.intensity;
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

    return totalPdf;
}
