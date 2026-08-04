// light_sampling.slang
//
// Geometry-only light sampling shared by the path tracer (pt_sample_lights.slang)
// and the raster deferred-lighting pass (deferred_direct_lighting.cs.slang).
//
// Rationale: the path tracer's sampleLights() interleaves geometry sampling
// with shadow TraceRay calls. The deferred pass has no TLAS, so it needs only
// the geometry part. This module factors the per-light-type geometry sampling
// into a single function returning direction/intensity/distance/pdf with NO
// shadow ray, so both consumers reuse the same math.
//
// pt_sample_lights.slang still owns its own copy (it adds TraceRay + dome-light
// skipping); refactoring it to call here is a follow-up. For now this is the
// authoritative geometry math, kept identical to pt_sample_lights.slang so a
// deferred-lit frame matches a path-traced single-bounce frame (modulo shadow).

import Scene.Lights.LightData;

static const float PI_LIGHT = 3.14159265359;

struct LightGeometrySample
{
    float3 direction;   ///< Normalized direction TOWARD the light (world space)
    float3 intensity;   ///< Light intensity with geometric/attenuation applied
    float  distance;    ///< Distance to the light (world units)
    float  pdf;         ///< PDF of selecting this light + sample point
    bool   valid;       ///< Whether the sample is valid (NdotL > 0, etc.)
};

/// Cosine-weighted hemisphere sampling (for lights enclosing the surface).
float3 cosineSampleHemisphere(float xi1, float xi2, float3 normal)
{
    float r = sqrt(xi1);
    float phi = 2.0 * PI_LIGHT * xi2;
    float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - xi1)));

    float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return localDir.x * tangent + localDir.y * bitangent + localDir.z * normal;
}

/// Sample a single analytic light's geometry (no shadow ray).
///
/// \param light           The light to sample.
/// \param sampleableCount Number of non-dome lights (selection PDF = 1/this).
/// \param hitPos          World-space shaded point.
/// \param hitNormal       World-space shading normal.
/// \param xi1, xi2        Random samples in [0,1) for area-light sampling.
LightGeometrySample sampleLightGeometry(
    LightData light,
    uint sampleableCount,
    float3 hitPos,
    float3 hitNormal,
    float xi1,
    float xi2)
{
    LightGeometrySample s;
    s.valid = false;
    s.direction = float3(0, 0, 0);
    s.intensity = float3(0, 0, 0);
    s.distance = 0.0;
    s.pdf = 0.0;

    if (sampleableCount == 0)
        return s;

    float lightPdf = 1.0 / float(sampleableCount);
    float3 lightDir;
    float lightDistance;

    if (light.type == uint(LightType::Point)) {
        lightDir = light.posW - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        if (NdotL > 0.0) {
            float attenuation = 1.0 / (lightDistance * lightDistance);
            s.direction = lightDir;
            s.intensity = light.intensity * attenuation;
            s.distance = lightDistance;
            s.pdf = lightPdf;
            s.valid = true;
        }
    }
    else if (light.type == uint(LightType::Sphere)) {
        float3 wc = light.posW - hitPos;
        float dist = length(wc);
        float3 wcN = wc / dist;

        float radius = sqrt(light.surfaceArea / (4.0 * PI_LIGHT));
        float3 sphereRadiance = light.intensity / PI_LIGHT;

        if (dist <= radius) {
            float3 dir = cosineSampleHemisphere(xi1, xi2, hitNormal);
            s.direction = dir;
            s.intensity = sphereRadiance;
            s.distance = dist;
            s.pdf = lightPdf;
            s.valid = true;
        }
        else {
            float sinThetaMax = min(radius / dist, 1.0);
            float cosThetaMax = sqrt(1.0 - sinThetaMax * sinThetaMax);

            float cosTheta = (1.0 - xi1) + xi1 * cosThetaMax;
            float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
            float phi = 2.0 * PI_LIGHT * xi2;

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

                float conePdf = 1.0 / (2.0 * PI_LIGHT * (1.0 - cosThetaMax));
                s.direction = lightDir;
                s.intensity = sphereRadiance;
                s.distance = max(0.001, lightDistance);
                s.pdf = lightPdf * conePdf;
                s.valid = true;
            }
        }
    }
    else if (light.type == uint(LightType::Distant)) {
        lightDir = -light.dirW;
        lightDistance = 1e4;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        if (NdotL > 0.0) {
            s.direction = lightDir;
            s.intensity = light.intensity;
            s.distance = lightDistance;
            s.pdf = lightPdf;
            s.valid = true;
        }
    }
    else if (light.type == uint(LightType::Rect)) {
        float u = xi1;
        float v = xi2;

        float3 localPoint = float3((u - 0.5), (v - 0.5), 0.0);
        float3 lightSamplePos = light.posW + localPoint.x * light.tangent +
                                localPoint.y * light.bitangent;

        lightDir = lightSamplePos - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        float LdotN = max(0.0, dot(-lightDir, light.dirW));

        if (LdotN > 0.0) {
            float geometricTerm = (NdotL * LdotN) / (lightDistance * lightDistance);
            float areaPdf = 1.0 / light.surfaceArea;
            s.direction = lightDir;
            s.intensity = light.intensity * geometricTerm;
            s.distance = lightDistance;
            s.pdf = lightPdf * areaPdf;
            s.valid = true;
        }
    }
    else if (light.type == uint(LightType::Disc)) {
        float r = sqrt(xi1);
        float theta = 2.0 * PI_LIGHT * xi2;

        float2 localPoint = float2(r * cos(theta), r * sin(theta));
        float3 lightSamplePos = light.posW + localPoint.x * light.tangent +
                                localPoint.y * light.bitangent;

        lightDir = lightSamplePos - hitPos;
        lightDistance = length(lightDir);
        lightDir /= lightDistance;

        float NdotL = max(0.0, dot(hitNormal, lightDir));
        float LdotN = max(0.0, dot(-lightDir, light.dirW));

        if (NdotL > 0.0 && LdotN > 0.0) {
            float geometricTerm = (NdotL * LdotN) / (lightDistance * lightDistance);
            float areaPdf = 1.0 / light.surfaceArea;
            s.direction = lightDir;
            s.intensity = light.intensity * geometricTerm;
            s.distance = max(0.0, lightDistance);
            s.pdf = lightPdf * areaPdf;
            s.valid = true;
        }
    }

    return s;
}
