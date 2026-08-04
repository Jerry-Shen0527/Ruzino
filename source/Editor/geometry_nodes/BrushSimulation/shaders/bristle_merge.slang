// Merge bristle accumulation grids into main simulation grids — 3D version
// N*N*D threads, one per 3D cell. Applies brightness-preserving color mixing.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);
StructuredBuffer<float> bristle_density : register(t0);
StructuredBuffer<float> bristle_vel_x : register(t1);
StructuredBuffer<float> bristle_vel_y : register(t2);
StructuredBuffer<float> bristle_vel_z : register(t3);
StructuredBuffer<float> bristle_color_r : register(t4);
StructuredBuffer<float> bristle_color_y : register(t5);
StructuredBuffer<float> bristle_color_b : register(t6);
RWStructuredBuffer<float> density : register(u0);
RWStructuredBuffer<float> color_r : register(u1);
RWStructuredBuffer<float> color_y : register(u2);
RWStructuredBuffer<float> color_b : register(u3);
RWStructuredBuffer<float> vel_x : register(u4);
RWStructuredBuffer<float> vel_y : register(u5);
RWStructuredBuffer<float> vel_z : register(u6);
RWStructuredBuffer<float> wetness : register(u7);
RWStructuredBuffer<float> oil_density : register(u8);  // §6 oil density (per-cell)

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // Window-relative: dispatch over the active window, not the full grid.
    WindowCell c;
    if (!window_map(cb, tid.x, c)) return;

    // Two index spaces:
    //   - win_idx: window-local, into the WINDOW-SIZED bristle/particle
    //     accumulation grids (Group B/C). Built from c.lx/ly/lz.
    //   - gidx:    global, into the FULL-GRID main sim fields (Group A:
    //     density/color/vel/wetness/oil_density). That's c.idx.
    int WSX = cb.window_size_x;
    uint win_idx = c.lz * uint(WSX * WSX) + c.ly * uint(WSX) + c.lx;
    uint gidx = c.idx;

    float bd = bristle_density[win_idx];
    if (bd < 1e-6) return;

    float bvx = bristle_vel_x[win_idx];
    float bvy = bristle_vel_y[win_idx];
    float bvz = bristle_vel_z[win_idx];

    // VELOCITY-ONLY coupling (paper §4.3 line 209: "we rasterize them into
    // density and velocity fields, and combine them with the existing
    // fields"). This merge combines particle momentum into the grid velocity
    // field so the joint pressure projection (§4.2) couples particle and grid
    // liquid. It does NOT permanently write paint MASS/pigment into the grid.
    //
    // Paper §4.3: the density+velocity combine is a TRANSIENT joint field for
    // pressure projection — "Rather than performing viscosity and pressure
    // projection steps on the original velocity field, we perform them on the
    // joint velocity field." The permanent paint→grid transfer is the §5.2
    // grid↔particle transfer (particle_to_grid Eq.16): a particle deposits
    // its mass into the grid only when it drifts beyond D0 of the bristles.
    // Writing density here every frame created a non-conservative loop
    // (supply refills samples → emit particles → rasterize → += grid density
    // → grid_to_particle converts back → repeat) that minted mass each frame
    // and bloomed into an unbounded black blob.
    vel_x[gidx] += bvx * bd;
    vel_y[gidx] += bvy * bd;
    vel_z[gidx] += bvz * bd;
}
