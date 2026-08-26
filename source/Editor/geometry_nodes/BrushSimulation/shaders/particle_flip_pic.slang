// FLIP/PIC velocity update — Wetbrush Eq.11  (3D)
// After pressure projection, update particle velocities from grid
// v_new = gamma * v_grid_new + (1-gamma) * (v_particle + v_grid_new - v_grid_old)

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<uint> ptcl_alive : register(t1);
StructuredBuffer<float> vel_x_old : register(t2);  // pre-projection
StructuredBuffer<float> vel_y_old : register(t3);
StructuredBuffer<float> vel_z_old : register(t4);
StructuredBuffer<float> vel_x_new : register(t5);  // post-projection
StructuredBuffer<float> vel_y_new : register(t6);
StructuredBuffer<float> vel_z_new : register(t7);
RWStructuredBuffer<float4> ptcl_vel : register(u0);

// Trilinear sample of a WINDOW-LOCAL scalar field (vel_old/vel_new — the
// window-sized velocity family) at world position. Outside the active window
// the value is 0: no liquid motion outside the window (paper §4.2).
// Window-local fetch with outside-window → 0 (velocity family).
float fetch_win(StructuredBuffer<float> field, int gx, int gy, int gz)
{
    int w = win_field_idx(
        gx, gy, gz, cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    return (w >= 0) ? field[w] : 0.0;
}

float sample_grid_3d(StructuredBuffer<float> field, float3 pos)
{
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size - 0.5;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size - 0.5;
    float gz = (pos.z - cb.grid_center_z + half_h) / cell_z - 0.5;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    int iz = int(floor(gz));
    float fx = gx - float(ix);
    float fy = gy - float(iy);
    float fz = gz - float(iz);


    float c000 = fetch_win(field, ix, iy, iz), c100 = fetch_win(field, ix+1, iy, iz);
    float c010 = fetch_win(field, ix, iy+1, iz), c110 = fetch_win(field, ix+1, iy+1, iz);
    float c001 = fetch_win(field, ix, iy, iz+1), c101 = fetch_win(field, ix+1, iy, iz+1);
    float c011 = fetch_win(field, ix, iy+1, iz+1), c111 = fetch_win(field, ix+1, iy+1, iz+1);

    float c00 = lerp(c000, c100, fx);
    float c10 = lerp(c010, c110, fx);
    float c01 = lerp(c001, c101, fx);
    float c11 = lerp(c011, c111, fx);
    float c0 = lerp(c00, c10, fy);
    float c1 = lerp(c01, c11, fy);
    return lerp(c0, c1, fz);
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float3 pos = ptcl_pos[idx].xyz;
    float3 v_particle = ptcl_vel[idx].xyz;

    float3 v_old = float3(
        sample_grid_3d(vel_x_old, pos),
        sample_grid_3d(vel_y_old, pos),
        sample_grid_3d(vel_z_old, pos));
    float3 v_new_grid = float3(
        sample_grid_3d(vel_x_new, pos),
        sample_grid_3d(vel_y_new, pos),
        sample_grid_3d(vel_z_new, pos));

    // FLIP/PIC blend (Eq.11)
    float gamma = cb.flip_gamma;
    float3 v_flip = v_particle + v_new_grid - v_old;
    float3 v_pic = v_new_grid;
    ptcl_vel[idx] = float4(gamma * v_pic + (1.0 - gamma) * v_flip, 0.0);
}
