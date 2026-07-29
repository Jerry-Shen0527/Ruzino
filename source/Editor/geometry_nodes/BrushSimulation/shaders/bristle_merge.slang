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
    float bcr = bristle_color_r[win_idx];
    float bcy = bristle_color_y[win_idx];
    float bcb = bristle_color_b[win_idx];

    // Normalize accumulated color by density
    float3 new_ryb = float3(bcr, bcy, bcb) / (bd + 1e-8);
    float3 old_ryb = float3(color_r[gidx], color_y[gidx], color_b[gidx]);

    float old_density = density[gidx];
    float new_density = bd;

    // Brightness-preserving color mix (Algorithm 2)
    float3 mixed = color_mix(new_ryb, old_ryb, new_density, old_density);

    // Update fields.
    //
    // Density: paper §5.2 Eq.16 is ρ_c += m_k·ΣW. `bd` (bristle_density) is
    // already the rasterized, mass-weighted density accumulation from
    // particles — it IS the m_k·ΣW term. Adding it directly is the conserved
    // transfer. (The previous `bd * cb.ink_amount` was a net-injection bug:
    // it multiplied the already-mass-weighted value by a user knob, breaking
    // conservation and driving unbounded growth. ink_amount's correct role is
    // the per-frame supply refill in deposit.cpp, the paint source.)
    density[gidx] = density[gidx] + bd;
    vel_x[gidx] += bvx * bd;
    vel_y[gidx] += bvy * bd;
    vel_z[gidx] += bvz * bd;
    color_r[gidx] = mixed.x;
    color_y[gidx] = mixed.y;
    color_b[gidx] = mixed.z;
    wetness[gidx] = min(wetness[gidx] + bd * 0.5, 1.0f);
    // Oil density relaxes toward base as bristle paint merges in (§6).
    oil_density[gidx] += (cb.oil_density_base - oil_density[gidx]) * bd * 0.5;
}
