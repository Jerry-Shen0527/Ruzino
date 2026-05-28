// Particle rasterization — splat particle density/velocity to accumulation grids
// max_particles threads, alive only

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float2> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_color : register(t1);
StructuredBuffer<float2> ptcl_vel : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
RWStructuredBuffer<float> ptcl_density : register(u0);
RWStructuredBuffer<float> ptcl_vel_x : register(u1);
RWStructuredBuffer<float> ptcl_vel_y : register(u2);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float2 pos = ptcl_pos[idx];
    float4 col = ptcl_color[idx];
    float2 vel = ptcl_vel[idx];

    float half_p = cb.paper_size * 0.5;
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size;

    int ix = int(floor(gx));
    int iy = int(floor(gy));

    // Bilinear splat to 2x2 cells
    float fx = gx - float(ix);
    float fy = gy - float(iy);

    float deposit = col.w * 0.1;

    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            int nx = ix + dx;
            int ny = iy + dy;
            if (nx < 0 || nx >= cb.grid_res || ny < 0 || ny >= cb.grid_res)
                continue;

            float w = ((dx == 0) ? (1.0 - fx) : fx) * ((dy == 0) ? (1.0 - fy) : fy);
            uint gi = grid_idx(uint(nx), uint(ny), cb.grid_res);

            ptcl_density[gi] += deposit * w;
            ptcl_vel_x[gi] += vel.x * deposit * w;
            ptcl_vel_y[gi] += vel.y * deposit * w;
        }
    }
}
