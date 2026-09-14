// Virtual pipes, pass 3 of 4 (Mei, Decaudin & Hu 2007): erosion /
// deposition. Sediment carrying capacity comes from the (terrain + water)
// surface tilt and the flow speed; deficits dissolve terrain into the water
// column, surpluses deposit. Cumulative dissolved amount feeds the wear map
// (the rock-exposure mask for downstream texturing).

#include "terrain_common.slangh"

ConstantBuffer<TerrainConstants> cb : register(b0);

StructuredBuffer<float> height : register(t0);      // current terrain
StructuredBuffer<float> water : register(t1);       // new water depth
StructuredBuffer<float> sediment : register(t2);    // suspended load
StructuredBuffer<float> vel_u : register(t3);
StructuredBuffer<float> vel_v : register(t4);

RWStructuredBuffer<float> height_out : register(u0);
RWStructuredBuffer<float> sediment_mid : register(u1);
RWStructuredBuffer<float> wear : register(u2);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint res = uint(cb.res);
    if (dtid.x >= res * res) return;
    uint x = dtid.x % res;
    uint y = dtid.x / res;
    uint i = dtid.x;

    // Tilt of (terrain + water): central differences on the clamped
    // neighbourhood, sin(alpha) without an atan (paper eq. 8).
    uint xl = (x > 0) ? x - 1 : x;
    uint xr = (x < res - 1) ? x + 1 : x;
    uint yd = (y > 0) ? y - 1 : y;
    uint yu = (y < res - 1) ? y + 1 : y;

    float sl = height[y * res + xl] + water[y * res + xl];
    float sr = height[y * res + xr] + water[y * res + xr];
    float sd = height[yd * res + x] + water[yd * res + x];
    float su = height[yu * res + x] + water[yu * res + x];

    float gx = (sr - sl) / (float(xr - xl) * 2.0) * cb.inv_cell;
    float gy = (su - sd) / (float(yu - yd) * 2.0) * cb.inv_cell;
    float tangent = sqrt(gx * gx + gy * gy);
    float sin_alpha = tangent / sqrt(1.0 + tangent * tangent);

    float speed = sqrt(vel_u[i] * vel_u[i] + vel_v[i] * vel_v[i]);
    float capacity =
        cb.sediment_capacity * sin_alpha * speed * water[i];

    if (capacity > sediment[i]) {
        float delta = min(
            cb.erosion_rate * (capacity - sediment[i]) * cb.dt,
            cb.max_delta);
        height_out[i] = height[i] - delta;
        sediment_mid[i] = sediment[i] + delta;
        wear[i] += delta;
    }
    else {
        float delta = min(
            cb.deposition_rate * (sediment[i] - capacity) * cb.dt,
            sediment[i]);
        height_out[i] = height[i] + delta;
        sediment_mid[i] = sediment[i] - delta;
    }
}
