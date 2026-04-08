import callable_data;
import Scene.BindlessMaterial;
import utils.Spectrum;

// Default dome light evaluation callable
// This provides the same behavior as the built-in dome light evaluation
// but can be customized by users
[shader("callable")]
void eval_dome_light_default(inout DomeLightCallableData data)
{
    float3 envColor = float3(0, 0, 0);

    if (data.textureIndex >= 0) {
        // Sample environment texture using spherical coordinates
        // Convert direction to spherical coordinates (latitude-longitude mapping)
        float phi = atan2(data.localDirection.z, data.localDirection.x);
        float theta = acos(clamp(data.localDirection.y, -1.0, 1.0));

        // Convert to UV coordinates [0,1]
        float u = (phi + 3.14159265359) / (2.0 * 3.14159265359);
        float v = theta / 3.14159265359;

        // Sample from bindless texture array
        // Use trilinear filtering (sampler index 1 = clamp + linear)
        float4 texColor =
            t_BindlessTextures[data.textureIndex].SampleLevel(
                samplers[1], float2(u, v), 0);
        envColor = texColor.rgb * data.intensity;
    }
    else {
        // No texture, use solid color intensity
        envColor = data.intensity;
    }

    data.envColor = Spectrum(envColor);
}
