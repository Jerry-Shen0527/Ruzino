// Transfer distant slow particles back to grid
// max_particles threads. Deposits density/color and kills the particle.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float2> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_color : register(t1);
StructuredBuffer<uint> ptcl_alive : register(t2);
RWStructuredBuffer<float> density : register(u0);
RWStructuredBuffer<float> color_r : register(u1);
RWStructuredBuffer<float> color_y : register(u2);
RWStructuredBuffer<float> color_b : register(u3);
RWStructuredBuffer<uint> ptcl_alive_out : register(u4);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) { ptcl_alive_out[idx] = 0; return; }

    float2 pos = ptcl_pos[idx];
    float d = length(pos - float2(cb.brush_pos_x, cb.brush_pos_y));

    // Only transfer particles that are far from brush
    if (d < cb.D0 * 1.5) {
        ptcl_alive_out[idx] = 1;
        return;
    }

    float4 col = ptcl_color[idx];

    float half_p = cb.paper_size * 0.5;
    int ix = int(floor((pos.x - cb.grid_center_x + half_p) / cb.cell_size));
    int iy = int(floor((pos.y - cb.grid_center_y + half_p) / cb.cell_size));

    if (ix >= 0 && ix < cb.grid_res && iy >= 0 && iy < cb.grid_res) {
        uint gi = grid_idx(uint(ix), uint(iy), cb.grid_res);
        density[gi] += col.w;
        color_r[gi] += col.x * col.w;
        color_y[gi] += col.y * col.w;
        color_b[gi] += col.z * col.w;
    }

    ptcl_alive_out[idx] = 0;
}
