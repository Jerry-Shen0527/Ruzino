// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z
//
// Global grid: neighbor access uses global coords/strides (res, res_z).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);
// Boundary-condition fields (Wetbrush §4.2): solid cells (dried paint)
// keep zero velocity and mirror pressure at their interface (Neumann BC).
// Empty cells are NOT solid even though their wetness is 0; the density
// gate enforces that (see fluid_jacobi.slang for the matching definition).
StructuredBuffer<float> wetness    : register(t1);
StructuredBuffer<float> density    : register(t2);
// Bristle rasterized density (paper §4.2 boundary condition). Window-sized.
StructuredBuffer<float> bristle_density : register(t3);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);
RWStructuredBuffer<float> vel_z : register(u2);

bool is_solid(uint idx)
{
    return density[idx] > 1e-6f && wetness[idx] < 0.01f;
}

// Brush interior boundary (paper §4.2). Global coords -> window-local lookup.
bool is_brush_g(uint gx, uint gy, uint gz)
{
    int lx = int(gx) - cb.window_origin_x;
    int ly = int(gy) - cb.window_origin_y;
    int lz = int(gz) - cb.window_origin_z;
    int WSX = cb.window_size_x;
    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX || lz < 0 || lz >= cb.window_size_z)
        return false;
    uint gi = uint(lz) * uint(WSX * WSX) + uint(ly) * uint(WSX) + uint(lx);
    return bristle_density[gi] > cb.brush_boundary_gate;
}

bool is_blocked(uint idx, uint gx, uint gy, uint gz)
{
    return is_solid(idx) || is_brush_g(gx, gy, gz);
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

    // Solid or brush-interior cells keep zero velocity (no update). Paper §4.2.
    if (is_blocked(idx, gx, gy, gz)) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
        return;
    }

    // ∂P/∂x — Neumann BC at solid/brush interfaces: a blocked neighbor
    // contributes pressure[idx] (mirror), so the gradient across the boundary
    // is zero. This prevents the fluid from being sucked toward blocked cells;
    // the fluid is simply deflected around the brush/dried paint.
    float px_lo = (gx > 0)             ? (is_blocked(idx - 1, gx - 1, gy, gz)    ? pressure[idx] : pressure[idx - 1])    : pressure[idx];
    float px_hi = (gx < (uint)RES - 1) ? (is_blocked(idx + 1, gx + 1, gy, gz)    ? pressure[idx] : pressure[idx + 1])    : pressure[idx];
    vel_x[idx] -= 0.5f * (px_hi - px_lo);

    // ∂P/∂y
    float py_lo = (gy > 0)             ? (is_blocked(idx - (uint)RES, gx, gy - 1, gz) ? pressure[idx] : pressure[idx - (uint)RES]) : pressure[idx];
    float py_hi = (gy < (uint)RES - 1) ? (is_blocked(idx + (uint)RES, gx, gy + 1, gz) ? pressure[idx] : pressure[idx + (uint)RES]) : pressure[idx];
    vel_y[idx] -= 0.5f * (py_hi - py_lo);

    // ∂P/∂z
    if (RESZ > 1) {
        float pz_lo = (gz > 0)             ? (is_blocked(idx - RNN, gx, gy, gz - 1) ? pressure[idx] : pressure[idx - RNN]) : pressure[idx];
        float pz_hi = (gz < (uint)RESZ - 1) ? (is_blocked(idx + RNN, gx, gy, gz + 1) ? pressure[idx] : pressure[idx + RNN]) : pressure[idx];
        vel_z[idx] -= 0.5f * (pz_hi - pz_lo);
    }
}
