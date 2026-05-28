// Emit new particles from grid cells near brush
// N*N threads. Grid cells within D0 of brush with high density emit particles.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float2> ptcl_pos : register(u1);
RWStructuredBuffer<float2> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int res = cb.grid_res;
    if (idx >= res * res) return;

    int x = idx % res;
    int y = idx / res;

    float half_p = cb.paper_size * 0.5;
    float wx = (float(x) + 0.5) * cb.cell_size - half_p + cb.grid_center_x;
    float wy = (float(y) + 0.5) * cb.cell_size - half_p + cb.grid_center_y;

    float2 cell_pos = float2(wx, wy);
    float2 brush_pos2 = float2(cb.brush_pos_x, cb.brush_pos_y);
    float d = length(cell_pos - brush_pos2);

    if (d > cb.D0) return;
    if (density[idx] < 0.2) return;

    uint slot;
    ptcl_counter.InterlockedAdd(0, 1, slot);
    slot = slot % uint(cb.max_particles);

    ptcl_pos[slot] = cell_pos;
    ptcl_vel[slot] = float2(0, 0);
    ptcl_color[slot] = float4(color_r[idx], color_y[idx], color_b[idx], density[idx]);
    ptcl_alive[slot] = 1;
}
