// Clear a field buffer to zero
// Used for bristle and particle accumulation grids

RWStructuredBuffer<float> field : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    field[tid.x] = 0.0;
}
