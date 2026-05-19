import pt_constants;
__exported import Scene.Lights.LightData;
StructuredBuffer<LightData> lightBuffer;

#include "utils/random.slangh"

struct LightSample {
    float3 direction;  // Normalized direction to light
    float3 intensity;  // Light intensity
    float pdf;         // PDF of selecting this light and sampling point
    bool valid;        // Whether the sample is valid
};

struct ShadowPayload {
    float3 hitPos;        // Hit position for continuation rays
    float3 rayDir;        // Ray direction for continuation
    float rayTMax;        // Maximum ray distance
    uint recursionDepth;  // Current recursion depth for opacity testing
    uint seed;
    bool isVisible;  // Whether light is visible or occluded
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
                0xFF,
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
                    0xFF,
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
                0xFF,
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
                0xFF,
                1,
                1,
                1,
                shadowRay,
                shadowPayload);
            seed = shadowPayload.seed;

            if (shadowPayload.isVisible) {
                float geometricTerm =
                    (NdotL * LdotN) / (lightDistance * lightDistance);
                areaPdf = 1.0 / light.surfaceArea;
                sample.direction = lightDir;
                sample.intensity = light.intensity * geometricTerm;
                sample.pdf = lightPdf * areaPdf;
                sample.valid = true;
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
                0xFF,
                1,
                1,
                1,
                shadowRay,
                shadowPayload);

            seed = shadowPayload.seed;

            if (shadowPayload.isVisible) {
                float geometricTerm =
                    (NdotL * LdotN) / (lightDistance * lightDistance);
                areaPdf = 1.0 / light.surfaceArea;
                sample.direction = lightDir;
                sample.intensity = light.intensity * geometricTerm;
                sample.pdf = lightPdf * areaPdf;
                sample.valid = true;
            }
        }
    }

    return sample;
}
