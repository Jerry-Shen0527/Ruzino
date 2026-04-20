
Texture2D<float4> Texture;
RWTexture2D<float4> Accumulated;

ConstantBuffer<int> CurrentSPP;
ConstantBuffer<int2> ImageSize;

[numthreads(32, 32, 1)]
void main(uint2 DTid: SV_DispatchThreadID)
{
    if (DTid.x >= ImageSize.x || DTid.y >= ImageSize.y) {
        return;
    }

    float4 color = Texture.Load(int3(DTid, 0));
    if (!isfinite(color.x) || !isfinite(color.y) || !isfinite(color.z) ||
        !isfinite(color.w)) {
        color = float4(0, 0, 1, 1);
    }
    if (isnan(color.x) || isnan(color.y) || isnan(color.z) || isnan(color.w)) {
        color = float4(0, 1, 1, 1);
    }
    float4 accumulated = Accumulated.Load(DTid);

    float3 newAccumulated = accumulated.xyz * (CurrentSPP) / (CurrentSPP + 1) +
                            color.xyz / (CurrentSPP + 1);

    Accumulated[DTid] = float4(newAccumulated, 1);
}
