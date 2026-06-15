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
    uint idx = tid.x;
    int total = cb.res * cb.res * cb.res_z;
    if (idx >= (uint)total) return;

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

    // Update fields
    float inv_bd = 1.0 / (bd + 1e-8);
    density[idx] += bd * cb.ink_amount;
    vel_x[idx] += bvx * inv_bd;
    vel_y[idx] += bvy * inv_bd;
    vel_z[idx] += bvz * inv_bd;
    color_r[idx] = mixed.x;
    color_y[idx] = mixed.y;
    color_b[idx] = mixed.z;
    wetness[idx] = min(wetness[idx] + bd * 0.5, 1.0f);
    // Oil density relaxes toward base as bristle paint merges in (§6).
    oil_density[idx] += (cb.oil_density_base - oil_density[idx]) * bd * 0.5;
}
