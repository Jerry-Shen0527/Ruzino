// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);
RWStructuredBuffer<float> vel_z : register(u2);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    int D = cb.res_z;
    int total = N * N * D;
    if (idx >= (uint)total) return;

    uint z = idx / (uint)(N * N);
    uint rem = idx % (uint)(N * N);
    uint y = rem / (uint)N;
    uint x = rem % (uint)N;

    // ∂P/∂x
    if (x > 0 && x < (uint)N - 1)
        vel_x[idx] -= 0.5f * (pressure[idx + 1] - pressure[idx - 1]);
    else if (x == 0)
        vel_x[idx] -= 0.5f * pressure[idx + 1];
    else
        vel_x[idx] += 0.5f * pressure[idx - 1];

    // ∂P/∂y
    if (y > 0 && y < (uint)N - 1)
        vel_y[idx] -= 0.5f * (pressure[idx + (uint)N] - pressure[idx - (uint)N]);
    else if (y == 0)
        vel_y[idx] -= 0.5f * pressure[idx + (uint)N];
    else
        vel_y[idx] += 0.5f * pressure[idx - (uint)N];

    // ∂P/∂z
    if (D > 1) {
        if (z > 0 && z < (uint)D - 1)
            vel_z[idx] -= 0.5f * (pressure[idx + (uint)(N*N)] - pressure[idx - (uint)(N*N)]);
        else if (z == 0)
            vel_z[idx] -= 0.5f * pressure[idx + (uint)(N*N)];
        else
            vel_z[idx] += 0.5f * pressure[idx - (uint)(N*N)];
    }
}
