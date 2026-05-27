// Subtract pressure gradient from velocity
// Wetbrush Algorithm 1: "u ← u - ∇P"
// Stam: u -= 0.5 * N * (P[i+1] - P[i-1])

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    if (idx >= (uint)N * N) return;

    uint x = idx % (uint)N;
    uint y = idx / (uint)N;

    float inv_N = 1.0f / (float)N;

    // ∂P/∂x
    if (x > 0 && x < (uint)N - 1)
        vel_x[idx] -= 0.5f * (pressure[idx + 1] - pressure[idx - 1]) * inv_N;
    else if (x == 0)
        vel_x[idx] -= 0.5f * pressure[idx + 1] * inv_N;
    else
        vel_x[idx] += 0.5f * pressure[idx - 1] * inv_N;

    // ∂P/∂y
    if (y > 0 && y < (uint)N - 1)
        vel_y[idx] -= 0.5f * (pressure[idx + (uint)N] - pressure[idx - (uint)N]) * inv_N;
    else if (y == 0)
        vel_y[idx] -= 0.5f * pressure[idx + (uint)N] * inv_N;
    else
        vel_y[idx] += 0.5f * pressure[idx - (uint)N] * inv_N;
}
