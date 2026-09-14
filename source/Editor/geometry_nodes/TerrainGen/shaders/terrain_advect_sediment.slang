// Virtual pipes, pass 4 of 4 (Mei, Decaudin & Hu 2007): advect the
// suspended sediment along the flow field (semi-Lagrangian backtrace).

#include "terrain_common.slangh"

ConstantBuffer<TerrainConstants> cb : register(b0);

StructuredBuffer<float> sediment : register(t0);
StructuredBuffer<float> vel_u : register(t1);
StructuredBuffer<float> vel_v : register(t2);

RWStructuredBuffer<float> sediment_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint res = uint(cb.res);
    if (dtid.x >= res * res) return;
    uint x = dtid.x % res;
    uint y = dtid.x / res;

    float gx = float(x) - vel_u[dtid.x] * cb.dt * cb.inv_cell;
    float gy = float(y) - vel_v[dtid.x] * cb.dt * cb.inv_cell;

    gx = clamp(gx, 0.0, float(res - 1));
    gy = clamp(gy, 0.0, float(res - 1));
    uint x0 = uint(gx);
    uint y0 = uint(gy);
    x0 = min(x0, res - 2);
    y0 = min(y0, res - 2);
    float fx = gx - float(x0);
    float fy = gy - float(y0);

    uint i00 = y0 * res + x0;
    float sampled = sediment[i00] * (1.0 - fx) * (1.0 - fy) +
                    sediment[i00 + 1] * fx * (1.0 - fy) +
                    sediment[i00 + res] * (1.0 - fx) * fy +
                    sediment[i00 + res + 1] * fx * fy;

    sediment_out[dtid.x] = sampled;
}
