// Virtual pipes, pass 1 of 4 (Mei, Decaudin & Hu 2007): per-cell pipe flux
// update with outflow scaling. Reads the CURRENT terrain/water/flux state;
// writes the NEXT fluxes. Map border neighbours have zero head — the edge is
// an open drain, water never pools against a wall.

#include "terrain_common.slangh"

ConstantBuffer<TerrainConstants> cb : register(b0);

StructuredBuffer<float> height : register(t0);
StructuredBuffer<float> water : register(t1);
StructuredBuffer<float> flux_l : register(t2);
StructuredBuffer<float> flux_r : register(t3);
StructuredBuffer<float> flux_t : register(t4);
StructuredBuffer<float> flux_b : register(t5);

RWStructuredBuffer<float> flux_l_out : register(u0);
RWStructuredBuffer<float> flux_r_out : register(u1);
RWStructuredBuffer<float> flux_t_out : register(u2);
RWStructuredBuffer<float> flux_b_out : register(u3);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint res = uint(cb.res);
    if (dtid.x >= res * res) return;
    uint x = dtid.x % res;
    uint y = dtid.x / res;
    uint i = dtid.x;

    float head_c = height[i] + water[i];

    float h_l = (x > 0) ? height[i - 1] + water[i - 1] : 0.0;
    float h_r = (x < res - 1) ? height[i + 1] + water[i + 1] : 0.0;
    float h_t = (y < res - 1) ? height[i + res] + water[i + res] : 0.0;
    float h_b = (y > 0) ? height[i - res] + water[i - res] : 0.0;

    float f_l = max(
        0.0,
        flux_l[i] +
            cb.dt * cb.pipe_area * cb.gravity * (head_c - h_l) * cb.inv_cell);
    float f_r = max(
        0.0,
        flux_r[i] +
            cb.dt * cb.pipe_area * cb.gravity * (head_c - h_r) * cb.inv_cell);
    float f_t = max(
        0.0,
        flux_t[i] +
            cb.dt * cb.pipe_area * cb.gravity * (head_c - h_t) * cb.inv_cell);
    float f_b = max(
        0.0,
        flux_b[i] +
            cb.dt * cb.pipe_area * cb.gravity * (head_c - h_b) * cb.inv_cell);

    // A cell cannot drain more water in one step than it holds.
    float total = f_l + f_r + f_t + f_b;
    float cell2 = 1.0 / (cb.inv_cell * cb.inv_cell);
    float max_volume = water[i] * cell2;
    float scale = 1.0;
    if (total > 0.0 && total * cb.dt > max_volume) {
        scale = max_volume / (total * cb.dt);
    }

    flux_l_out[i] = f_l * scale;
    flux_r_out[i] = f_r * scale;
    flux_t_out[i] = f_t * scale;
    flux_b_out[i] = f_b * scale;
}
