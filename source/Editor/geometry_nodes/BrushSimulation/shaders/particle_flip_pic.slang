// FLIP/PIC velocity update — Wetbrush Eq.11
// After pressure projection, update particle velocities from grid
// v_new = gamma * v_grid_new + (1-gamma) * (v_particle + v_grid_new - v_grid_old)

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float2> ptcl_pos : register(t0);
StructuredBuffer<uint> ptcl_alive : register(t1);
StructuredBuffer<float> vel_x_old : register(t2);  // pre-projection
StructuredBuffer<float> vel_y_old : register(t3);
StructuredBuffer<float> vel_x_new : register(t4);  // post-projection
StructuredBuffer<float> vel_y_new : register(t5);
RWStructuredBuffer<float2> ptcl_vel : register(u0);

float sample_grid(StructuredBuffer<float> field, float2 pos)
{
    float half_p = cb.paper_size * 0.5;
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size - 0.5;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size - 0.5;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    float fx = gx - float(ix);
    float fy = gy - float(iy);

    float v00 = (ix >= 0 && iy >= 0 && ix < cb.grid_res && iy < cb.grid_res)
        ? field[grid_idx(uint(ix), uint(iy), cb.grid_res)] : 0.0;
    float v10 = (ix+1 >= 0 && iy >= 0 && ix+1 < cb.grid_res && iy < cb.grid_res)
        ? field[grid_idx(uint(ix+1), uint(iy), cb.grid_res)] : 0.0;
    float v01 = (ix >= 0 && iy+1 >= 0 && ix < cb.grid_res && iy+1 < cb.grid_res)
        ? field[grid_idx(uint(ix), uint(iy+1), cb.grid_res)] : 0.0;
    float v11 = (ix+1 >= 0 && iy+1 >= 0 && ix+1 < cb.grid_res && iy+1 < cb.grid_res)
        ? field[grid_idx(uint(ix+1), uint(iy+1), cb.grid_res)] : 0.0;

    return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fy);
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float2 pos = ptcl_pos[idx];
    float2 v_particle = ptcl_vel[idx];

    float2 v_old = float2(
        sample_grid(vel_x_old, pos),
        sample_grid(vel_y_old, pos));
    float2 v_new_grid = float2(
        sample_grid(vel_x_new, pos),
        sample_grid(vel_y_new, pos));

    // FLIP/PIC blend (Eq.11)
    float gamma = cb.flip_gamma;
    float2 v_flip = v_particle + v_new_grid - v_old;
    float2 v_pic = v_new_grid;
    ptcl_vel[idx] = gamma * v_pic + (1.0 - gamma) * v_flip;
}
