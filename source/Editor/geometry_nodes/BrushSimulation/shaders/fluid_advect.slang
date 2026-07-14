// Semi-Lagrangian advection — Stam 1999, 3D version
// Wetbrush Section 4.2: "we advect all of the fields using the semi-Lagrangian method"
// 3D grid: res × res × res_z, with 3-component velocity (vel_x, vel_y, vel_z)
//
// Global grid: buffers are global-sized (res × res × res_z), so all indexing
// is GLOBAL. Trace-back is performed in global coordinates and clamped to the
// global grid bounds. The dispatch range is the active window, so only window
// cells are updated — cells outside keep their previous values.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> vel_x   : register(t1);
StructuredBuffer<float> vel_y   : register(t2);
StructuredBuffer<float> vel_z   : register(t3);

RWStructuredBuffer<float> field_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;          // global grid XY
    int D = cb.res_z;        // global grid Z

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Global coordinates and index.
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;

    // Velocity is stored normalized to the GLOBAL grid (dt0 = dt*N).
    float dt0 = cb.dt * (float)N;
    float dt_z = cb.dt * (float)D;

    // Trace back in GLOBAL coordinates.
    float px = (float)gx - dt0 * vel_x[idx];
    float py = (float)gy - dt0 * vel_y[idx];
    float pz = (float)gz - dt_z * vel_z[idx];

    // Clamp to GLOBAL grid bounds.
    float max_xy = (float)N - 1.5f;
    float max_z = (float)D - 1.5f;
    px = max(0.5f, min(px, max_xy));
    py = max(0.5f, min(py, max_xy));
    pz = max(0.5f, min(pz, max_z));

    // Trilinear interpolation over the GLOBAL buffer.
    int x0 = (int)px, y0 = (int)py, z0 = (int)pz;
    int x1 = min(x0 + 1, N - 1), y1 = min(y0 + 1, N - 1), z1 = min(z0 + 1, D - 1);
    float sx = px - (float)x0, sy = py - (float)y0, sz = pz - (float)z0;

    float c000 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z0, N, D)];
    float c100 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z0, N, D)];
    float c010 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z0, N, D)];
    float c110 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z0, N, D)];
    float c001 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z1, N, D)];
    float c101 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z1, N, D)];
    float c011 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z1, N, D)];
    float c111 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z1, N, D)];

    float c00 = lerp(c000, c100, sx);
    float c10 = lerp(c010, c110, sx);
    float c01 = lerp(c001, c101, sx);
    float c11 = lerp(c011, c111, sx);
    float c0 = lerp(c00, c10, sy);
    float c1 = lerp(c01, c11, sy);

    field_out[idx] = lerp(c0, c1, sz);
}
