RWStructuredBuffer<float> bufferR;
RWStructuredBuffer<float> bufferG;
RWStructuredBuffer<float> bufferB;
RWStructuredBuffer<float> bufferA;
StructuredBuffer<float4> inputColor;
StructuredBuffer<uint2> inputPixelID;
ConstantBuffer<uint> bufferLength;
ConstantBuffer<uint2> image_size;

[numthreads(64, 1, 1)]
void main(uint3 DTid: SV_DispatchThreadID)
{
    if (DTid.x >= bufferLength) {
        return;
    }
    uint2 pixelID = inputPixelID[DTid.x];
    float4 color = inputColor[DTid.x];

    uint index = pixelID.y * image_size.x +
                 pixelID.x;  // Assuming you have the width of the texture

    // Atomic add for float values using InterlockedCompareExchangeFloatBitwise
    float originalR, currentR, newR;
    float originalG, currentG, newG;
    float originalB, currentB, newB;
    float originalA, currentA, newA;

    // Red channel
    currentR = bufferR[index];
    do {
        originalR = currentR;
        newR = originalR + color.r;
        InterlockedCompareExchangeFloatBitwise(
            bufferR[index], originalR, newR, currentR);
    } while (currentR != originalR);

    // Green channel
    currentG = bufferG[index];
    do {
        originalG = currentG;
        newG = originalG + color.g;
        InterlockedCompareExchangeFloatBitwise(
            bufferG[index], originalG, newG, currentG);
    } while (currentG != originalG);

    // Blue channel
    currentB = bufferB[index];
    do {
        originalB = currentB;
        newB = originalB + color.b;
        InterlockedCompareExchangeFloatBitwise(
            bufferB[index], originalB, newB, currentB);
    } while (currentB != originalB);

    // Alpha channel
    currentA = bufferA[index];
    do {
        originalA = currentA;
        newA = originalA + color.a;
        InterlockedCompareExchangeFloatBitwise(
            bufferA[index], originalA, newA, currentA);
    } while (currentA != originalA);
}
