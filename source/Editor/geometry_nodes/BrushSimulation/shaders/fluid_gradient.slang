// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z
//
// Global grid: neighbor access uses global coords/strides (res, res_z).

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
    int RES  = cb.res;
    int RESZ = cb.res_z;
    uint RNN = (uint)(RES * RES);

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;

    // Solid cells keep zero velocity (no update).
    if (is_solid(idx)) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
        return;
    }

    // ∂P/∂x
    if (gx > 0 && gx < (uint)RES - 1)
        vel_x[idx] -= 0.5f * (pressure[idx + 1] - pressure[idx - 1]);
    else if (gx == 0)
        vel_x[idx] -= 0.5f * pressure[idx + 1];
    else
        vel_x[idx] += 0.5f * pressure[idx - 1];

    // ∂P/∂y
    if (gy > 0 && gy < (uint)RES - 1)
        vel_y[idx] -= 0.5f * (pressure[idx + (uint)RES] - pressure[idx - (uint)RES]);
    else if (gy == 0)
        vel_y[idx] -= 0.5f * pressure[idx + (uint)RES];
    else
        vel_y[idx] += 0.5f * pressure[idx - (uint)RES];

    // ∂P/∂z
    if (RESZ > 1) {
        if (gz > 0 && gz < (uint)RESZ - 1)
            vel_z[idx] -= 0.5f * (pressure[idx + RNN] - pressure[idx - RNN]);
        else if (gz == 0)
            vel_z[idx] -= 0.5f * pressure[idx + RNN];
        else
            vel_z[idx] += 0.5f * pressure[idx - RNN];
    }
}
