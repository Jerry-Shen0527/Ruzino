import stdlib.genslang.lib.mx_transform_uv;

void mx_image_color4(
    Texture2D texture,
    int layer,
    float4 defaultval,
    float2 texcoord,
    int uaddressmode,
    int vaddressmode,
    int filtertype,
    int framerange,
    int frameoffset,
    int frameendaction,
    float2 uv_scale,
    float2 uv_offset,
    out float4 result)
{
    float2 uv = mx_transform_uv(texcoord, uv_scale, uv_offset);

    // Calculate sampler index based on modes
    // Index calculation: (uaddressmode * 3) + filtertype
    // 0=wrap, 1=clamp, 2=mirror for address modes
    // 0=point, 1=linear, 2=aniso for filter types
    int samplerIndex = (uaddressmode * 3) + filtertype;
    samplerIndex = clamp(samplerIndex, 0, 8);

    result = texture.SampleLevel(samplers[samplerIndex], uv, 0.0);
}
