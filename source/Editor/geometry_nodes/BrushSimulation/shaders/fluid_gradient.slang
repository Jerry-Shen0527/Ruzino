// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);
// Boundary-condition fields (Wetbrush §4.2): solid cells keep zero velocity.
StructuredBuffer<float> bristle_psi : register(t1);
StructuredBuffer<float> wetness    : register(t2);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);
RWStructuredBuffer<float> vel_z : register(u2);

bool is_solid(uint idx)
{
    return bristle_psi[idx] > 0.5 || wetness[idx] < 0.01;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    int D = cb.res_z;
    uint NN = (uint)(N * N);

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint x = c.x, y = c.y, z = c.z, idx = c.idx;

    // Solid cells keep zero velocity (no update).
    if (is_solid(idx)) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
        return;
    }

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
            vel_z[idx] -= 0.5f * (pressure[idx + NN] - pressure[idx - NN]);
        else if (z == 0)
            vel_z[idx] -= 0.5f * pressure[idx + NN];
        else
            vel_z[idx] += 0.5f * pressure[idx - NN];
    }
}
