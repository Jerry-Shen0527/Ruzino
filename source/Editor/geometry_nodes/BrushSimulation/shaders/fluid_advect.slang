// Semi-Lagrangian advection — Stam 1999, 3D version
// Wetbrush Section 4.2: "we advect all of the fields using the semi-Lagrangian method"
// 3D grid: res × res × res_z, with 3-component velocity (vel_x, vel_y, vel_z)
//
// Window-relative: buffers are allocated at active-window size (WIN_XY × WIN_XY
// × WIN_Z), so all indexing is window-LOCAL. Trace-back is performed in
// window-local coordinates and clamped to the window — cells outside the
// window are part of the committed 2D canvas layer and not advected here.
// (Wetbrush's high viscosity means paint barely moves once deposited, so
// clamping at the window edge introduces only minor boundary dissipation.)

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
    int N = cb.res;          // global grid XY (for velocity normalization scale)
    int D = cb.res_z;        // global grid Z

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Window-local coordinates and index.
    uint lx = c.lx, ly = c.ly, lz = c.lz;
    uint idx = c.idx;

    int WIN = cb.window_size_x;   // window XY size
    int WINZ = cb.window_size_z;  // window Z size

    // Velocity is stored normalized to the GLOBAL grid (dt0 = dt*N), so the
    // trace-back displacement in cell units is the same in local coords.
    float dt0 = cb.dt * (float)N;
    float dt_z = cb.dt * (float)D;

    // Trace back in WINDOW-LOCAL coordinates.
    float px = (float)lx - dt0 * vel_x[idx];
    float py = (float)ly - dt0 * vel_y[idx];
    float pz = (float)lz - dt_z * vel_z[idx];

    // Clamp to WINDOW bounds (not global). Outside-window cells are not in
    // this buffer; treat them as zero-dissipation boundaries.
    float max_xy = (float)WIN - 1.5f;
    float max_z = (float)WINZ - 1.5f;
    px = max(0.5f, min(px, max_xy));
    py = max(0.5f, min(py, max_xy));
    pz = max(0.5f, min(pz, max_z));

    // Trilinear interpolation over the WINDOW buffer.
    int x0 = (int)px, y0 = (int)py, z0 = (int)pz;
    int x1 = min(x0 + 1, WIN - 1), y1 = min(y0 + 1, WIN - 1), z1 = min(z0 + 1, WINZ - 1);
    float sx = px - (float)x0, sy = py - (float)y0, sz = pz - (float)z0;

    float c000 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z0, WIN, WINZ)];
    float c100 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z0, WIN, WINZ)];
    float c010 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z0, WIN, WINZ)];
    float c110 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z0, WIN, WINZ)];
    float c001 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z1, WIN, WINZ)];
    float c101 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z1, WIN, WINZ)];
    float c011 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z1, WIN, WINZ)];
    float c111 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z1, WIN, WINZ)];

    float c00 = lerp(c000, c100, sx);
    float c10 = lerp(c010, c110, sx);
    float c01 = lerp(c001, c101, sx);
    float c11 = lerp(c011, c111, sx);
    float c0 = lerp(c00, c10, sy);
    float c1 = lerp(c01, c11, sy);

    field_out[idx] = lerp(c0, c1, sz);
}
