// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z
//
// Window-relative: neighbor access uses window-local coords/strides.

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
    int WIN  = cb.window_size_x;
    int WINZ = cb.window_size_z;
    uint WNN = (uint)(WIN * WIN);

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint lx = c.lx, ly = c.ly, lz = c.lz;
    uint idx = c.idx;

    // Solid cells keep zero velocity (no update).
    if (is_solid(idx)) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
        return;
    }

    // ∂P/∂x
    if (lx > 0 && lx < (uint)WIN - 1)
        vel_x[idx] -= 0.5f * (pressure[idx + 1] - pressure[idx - 1]);
    else if (lx == 0)
        vel_x[idx] -= 0.5f * pressure[idx + 1];
    else
        vel_x[idx] += 0.5f * pressure[idx - 1];

    // ∂P/∂y
    if (ly > 0 && ly < (uint)WIN - 1)
        vel_y[idx] -= 0.5f * (pressure[idx + (uint)WIN] - pressure[idx - (uint)WIN]);
    else if (ly == 0)
        vel_y[idx] -= 0.5f * pressure[idx + (uint)WIN];
    else
        vel_y[idx] += 0.5f * pressure[idx - (uint)WIN];

    // ∂P/∂z
    if (WINZ > 1) {
        if (lz > 0 && lz < (uint)WINZ - 1)
            vel_z[idx] -= 0.5f * (pressure[idx + WNN] - pressure[idx - WNN]);
        else if (lz == 0)
            vel_z[idx] -= 0.5f * pressure[idx + WNN];
        else
            vel_z[idx] += 0.5f * pressure[idx - WNN];
    }
}
