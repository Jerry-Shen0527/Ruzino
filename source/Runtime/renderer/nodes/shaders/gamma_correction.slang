Texture2D<float4> InputTexture;
RWTexture2D<float4> OutputTexture;

ConstantBuffer<float> Gamma;
ConstantBuffer<int2> ImageSize;

[numthreads(32, 32, 1)]
void main(uint2 DTid: SV_DispatchThreadID)
{
    if (DTid.x >= ImageSize.x || DTid.y >= ImageSize.y) {
        return;
    }

    float4 color = InputTexture.Load(int3(DTid, 0));

    // Apply gamma correction to RGB channels only, preserve alpha
    float3 corrected = pow(max(color.rgb, 0.0), 1.0 / Gamma);

    OutputTexture[DTid] = float4(corrected, color.a);
}
