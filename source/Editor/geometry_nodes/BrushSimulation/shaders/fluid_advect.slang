// Semi-Lagrangian advection — Stam 1999, 3D version
// Wetbrush Section 4.2: "we advect all of the fields using the semi-Lagrangian method"
// 3D grid: res × res × res_z, with 3-component velocity (vel_x, vel_y, vel_z)
//
// Window-local buffers (paper §4.2): the velocity family is window-SIZED, so
// both the backtrace velocity samples and (when advecting velocity itself)
// field_in go through win_field_idx with outside→0 — there is no liquid motion
// outside the active window. Canvas scalars (density/color/wetness/oil) are
// GLOBAL fields: field_in reads globally (cb.advect_field_window_local == 0).
// field_out is ALWAYS the window-sized tmp; the host copies it back into the
// global canvas field (field_copy_window mode 2).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> vel_x   : register(t1);
StructuredBuffer<float> vel_y   : register(t2);
StructuredBuffer<float> vel_z   : register(t3);

RWStructuredBuffer<float> field_out : register(u0);

// Velocity components are window-local: 0 outside the active window.
float fetch_vel(StructuredBuffer<float> f, int gx, int gy, int gz)
{
    int w = win_field_idx(
        gx, gy, gz, cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    return (w >= 0) ? f[w] : 0.0;
}

// field_in: global canvas field or window-local velocity, per
// cb.advect_field_window_local.
float fetch_in(StructuredBuffer<float> f, int gx, int gy, int gz)
{
    if (cb.advect_field_window_local != 0)
        return fetch_vel(f, gx, gy, gz);
    return f[grid_idx_3d(uint(gx), uint(gy), uint(gz), cb.res, cb.res_z)];
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;          // global grid XY
    int D = cb.res_z;        // global grid Z
    int WSX = cb.window_size_x;
    int WSZ = cb.window_size_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Global coordinates and index.
    uint gx = c.x, gy = c.y, gz = c.z;

    // Velocity is in WORLD units (u/s) — the same convention as every
    // producer and consumer of this field (bristle rasterize, swarm merge,
    // gravity body force, projection wall BC, FLIP/PIC transfer, particle
    // drag). Convert to a CELL displacement per axis for the backtrace:
    //   cells = world_vel * dt / cell_size_axis.
    // (The old dt*N / dt*D multipliers treated the field as cells/s, which
    // no writer ever produced — every producer writes world u/s — so all
    // advection ran at an arbitrary scale and the y/z axes disagreed with x.)
    float cell_z = cb.height_extent / (float)D;
    float dtx = cb.dt / cb.cell_size;
    float dtz = cb.dt / cell_z;

    // Trace back in GLOBAL coordinates.
    float px = (float)gx - dtx * fetch_vel(vel_x, int(gx), int(gy), int(gz));
    float py = (float)gy - dtx * fetch_vel(vel_y, int(gx), int(gy), int(gz));
    float pz = (float)gz - dtz * fetch_vel(vel_z, int(gx), int(gy), int(gz));

    // Clamp to GLOBAL grid bounds.
    float max_xy = (float)N - 1.5f;
    float max_z = (float)D - 1.5f;
    px = max(0.5f, min(px, max_xy));
    py = max(0.5f, min(py, max_xy));
    pz = max(0.5f, min(pz, max_z));

    // Trilinear interpolation over the 8 corner cells.
    int x0 = (int)px, y0 = (int)py, z0 = (int)pz;
    int x1 = min(x0 + 1, N - 1), y1 = min(y0 + 1, N - 1), z1 = min(z0 + 1, D - 1);
    float sx = px - (float)x0, sy = py - (float)y0, sz = pz - (float)z0;

    float c000 = fetch_in(field_in, x0, y0, z0);
    float c100 = fetch_in(field_in, x1, y0, z0);
    float c010 = fetch_in(field_in, x0, y1, z0);
    float c110 = fetch_in(field_in, x1, y1, z0);
    float c001 = fetch_in(field_in, x0, y0, z1);
    float c101 = fetch_in(field_in, x1, y0, z1);
    float c011 = fetch_in(field_in, x0, y1, z1);
    float c111 = fetch_in(field_in, x1, y1, z1);

    float c00 = lerp(c000, c100, sx);
    float c10 = lerp(c010, c110, sx);
    float c01 = lerp(c001, c101, sx);
    float c11 = lerp(c011, c111, sx);
    float c0 = lerp(c00, c10, sy);
    float c1 = lerp(c01, c11, sy);

    // Write window-locally (field_out is the window-sized tmp).
    int w0 = wnb(c, 0, 0, 0, WSX, WSZ);
    field_out[w0] = lerp(c0, c1, sz);
}
