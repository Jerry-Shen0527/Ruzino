// Virtual pipes, pass 2 of 4 (Mei, Decaudin & Hu 2007): water surface
// update (+ rain, - evaporation) and the depth-averaged flow velocity from
// the net pipe fluxes (paper eq. 6).

#include "terrain_common.slangh"

ConstantBuffer<TerrainConstants> cb : register(b0);

StructuredBuffer<float> water : register(t0);
StructuredBuffer<float> flux_l : register(t1);
StructuredBuffer<float> flux_r : register(t2);
StructuredBuffer<float> flux_t : register(t3);
StructuredBuffer<float> flux_b : register(t4);

RWStructuredBuffer<float> water_out : register(u0);
RWStructuredBuffer<float> vel_u : register(u1);
RWStructuredBuffer<float> vel_v : register(u2);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint res = uint(cb.res);
    if (dtid.x >= res * res) return;
    uint x = dtid.x % res;
    uint y = dtid.x / res;
    uint i = dtid.x;

    // Inflow = the scaled outflow of the four neighbours towards this cell.
    float inflow = 0.0;
    if (x > 0) inflow += flux_r[i - 1];
    if (x < res - 1) inflow += flux_l[i + 1];
    if (y > 0) inflow += flux_t[i - res];
    if (y < res - 1) inflow += flux_b[i + res];

    float outflow =
        flux_l[i] + flux_r[i] + flux_t[i] + flux_b[i];

    float depth = water[i] +
                  cb.dt * (inflow - outflow) * cb.inv_cell * cb.inv_cell +
                  cb.rain_rate * cb.dt;
    depth =
        max(0.0, depth) * max(0.0, 1.0 - cb.evaporation * cb.dt);
    water_out[i] = depth;

    float denom = max(depth, 1e-4) / cb.inv_cell;
    vel_u[i] = (flux_r[i] - flux_l[i]) / denom;
    vel_v[i] = (flux_t[i] - flux_b[i]) / denom;
}
