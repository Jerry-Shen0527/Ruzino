// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z
//
// Global grid: neighbor access uses global coords/strides (res, res_z).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);
// Boundary-condition fields (Wetbrush §4.2): solid cells (bristle-occupied
// OR dried-paint) keep zero velocity and mirror pressure at their interface
// (Neumann BC). Empty cells are NOT solid even though their wetness is 0;
// the density gate enforces that (see fluid_jacobi.slang for the matching
// definition).
StructuredBuffer<float> bristle_psi : register(t1);
StructuredBuffer<float> wetness    : register(t2);
StructuredBuffer<float> density    : register(t3);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);
RWStructuredBuffer<float> vel_z : register(u2);

bool is_solid(uint idx)
{
    return bristle_psi[idx] > 0.5 ||
           (density[idx] > 1e-6f && wetness[idx] < 0.01f);
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

    // ∂P/∂x — Neumann BC at solid interfaces: a solid neighbor contributes
    // pressure[idx] (mirror), so the gradient across a solid boundary is zero.
    // This prevents the fluid from being sucked toward solid cells (which had
    // pressure pinned low) — instead the fluid is simply blocked, the way a
    // wall deflects incoming liquid. Without this, dry paint acted like a
    // vacuum pulling new paint IN rather than pushing it up/aside.
    float px_lo = (gx > 0)             ? (is_solid(idx - 1)    ? pressure[idx] : pressure[idx - 1])    : pressure[idx];
    float px_hi = (gx < (uint)RES - 1) ? (is_solid(idx + 1)    ? pressure[idx] : pressure[idx + 1])    : pressure[idx];
    vel_x[idx] -= 0.5f * (px_hi - px_lo);

    // ∂P/∂y
    float py_lo = (gy > 0)             ? (is_solid(idx - (uint)RES) ? pressure[idx] : pressure[idx - (uint)RES]) : pressure[idx];
    float py_hi = (gy < (uint)RES - 1) ? (is_solid(idx + (uint)RES) ? pressure[idx] : pressure[idx + (uint)RES]) : pressure[idx];
    vel_y[idx] -= 0.5f * (py_hi - py_lo);

    // ∂P/∂z
    if (RESZ > 1) {
        float pz_lo = (gz > 0)             ? (is_solid(idx - RNN) ? pressure[idx] : pressure[idx - RNN]) : pressure[idx];
        float pz_hi = (gz < (uint)RESZ - 1) ? (is_solid(idx + RNN) ? pressure[idx] : pressure[idx + RNN]) : pressure[idx];
        vel_z[idx] -= 0.5f * (pz_hi - pz_lo);
    }
}
