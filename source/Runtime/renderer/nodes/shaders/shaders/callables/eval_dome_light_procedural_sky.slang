import callable_data;
#include "utils/random.slangh"
import utils.Spectrum;

// Procedural sky dome light callable
// Generates a physically-based sky with sun instead of using a texture
[shader("callable")]
void eval_dome_light_procedural_sky(inout DomeLightCallableData data)
{
    float3 rayDir = data.localDirection;

    // Sky gradient parameters
    float3 horizonColor = float3(0.8, 0.9, 1.0);
    float3 zenithColor = float3(0.2, 0.4, 0.8);
    float3 sunColor = float3(1.0, 0.95, 0.9);

    // Sun direction (can be parameterized via intensity or other means)
    float3 sunDir = normalize(float3(0.5, 0.5, 0.7));

    // Calculate sky color based on ray direction
    float skyBlend = pow(max(0.0, rayDir.z + 0.1), 0.8);
    float3 skyColor = lerp(horizonColor, zenithColor, skyBlend);

    // Add sun disk
    float sunDot = max(0.0, dot(rayDir, sunDir));
    float sunDisk = pow(sunDot, 512.0);  // Sharp sun disk
    float sunGlow = pow(sunDot, 32.0) * 0.3;  // Soft glow around sun

    skyColor += sunColor * (sunDisk * 50.0 + sunGlow);

    // Apply intensity scaling
    data.envColor = Spectrum(skyColor * data.intensity);
}
