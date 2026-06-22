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
    uint idx = c.idx;  // window-local index

    float bd = bristle_density[idx];
    if (bd < 1e-6) return;

    float bvx = bristle_vel_x[idx];
    float bvy = bristle_vel_y[idx];
    float bvz = bristle_vel_z[idx];
    float bcr = bristle_color_r[idx];
    float bcy = bristle_color_y[idx];
    float bcb = bristle_color_b[idx];

    // Normalize accumulated color by density
    float3 new_ryb = float3(bcr, bcy, bcb) / (bd + 1e-8);
    float3 old_ryb = float3(color_r[idx], color_y[idx], color_b[idx]);

    float old_density = density[idx];
    float new_density = bd;

    // Brightness-preserving color mix (Algorithm 2)
    float3 mixed = color_mix(new_ryb, old_ryb, new_density, old_density);

    // Update fields. Clamp accumulated density to prevent the numerical
    // explosion seen with large sweep deposit (the fluid advection can
    // concentrate mass into a few cells, amplifying it exponentially). A
    // density cap of ~3.0 is well above any realistic paint layer but stops
    // the feedback loop. This is also where Wetbrush's own saturation term
    // would kick in (§4.1 density constraint), approximated here as a hard cap.
    //
    // Velocity: deposit the bristle velocity MASS-WEIGHTED by bd (not divided
    // by bd). The old `bvx * inv_bd` normalized velocity to per-unit-density,
    // which blew up to millions when bd was small (thin sweep deposits) — the
    // huge velocity then drove advection to pile density into window corners.
    // Mass-weighted deposit keeps velocity bounded: a light deposit adds a
    // light velocity, matching how bristles impart momentum to paint.
    density[idx] = min(density[idx] + bd * cb.ink_amount, 3.0);
    vel_x[idx] += bvx * bd;
    vel_y[idx] += bvy * bd;
    vel_z[idx] += bvz * bd;
    color_r[idx] = mixed.x;
    color_y[idx] = mixed.y;
    color_b[idx] = mixed.z;
    wetness[idx] = min(wetness[idx] + bd * 0.5, 1.0f);
    // Oil density relaxes toward base as bristle paint merges in (§6).
    oil_density[idx] += (cb.oil_density_base - oil_density[idx]) * bd * 0.5;
}
