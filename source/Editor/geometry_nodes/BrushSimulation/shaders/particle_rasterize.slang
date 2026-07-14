// Particle rasterization — splat particle density/velocity/color to 3D accum grids
// max_particles threads, alive only. Trilinear splat to a 2×2×2 cell block.
// Low collision probability since particles are spread across the volume;
// rare races are compensated by merge normalization.
// Rasterizes RYB color channels weighted by particle mass for proper color transfer.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_color : register(t1);
StructuredBuffer<float4> ptcl_vel : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
RWStructuredBuffer<float> ptcl_density : register(u0);
RWStructuredBuffer<float> ptcl_vel_x : register(u1);
RWStructuredBuffer<float> ptcl_vel_y : register(u2);
RWStructuredBuffer<float> ptcl_vel_z : register(u3);
// Color channels: RYB weighted by mass (for merge into main grid)
RWStructuredBuffer<float> ptcl_color_r : register(u4);
RWStructuredBuffer<float> ptcl_color_y : register(u5);
RWStructuredBuffer<float> ptcl_color_b : register(u6);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float3 pos = ptcl_pos[idx].xyz;
    float4 col = ptcl_color[idx];  // RYB pigment + mass in w
    float3 vel = ptcl_vel[idx].xyz;

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size;
    float gz = (pos.z - cb.grid_center_z + half_h) / cell_z;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    int iz = int(floor(gz));

    // Trilinear splat to 2×2×2 cells
    float fx = gx - float(ix);
    float fy = gy - float(iy);
    float fz = gz - float(iz);

    float deposit = col.w * 0.1;
    float3 color_deposit = col.rgb * col.w * 0.1;

    int res = cb.grid_res, res_z = cb.grid_res_z;
    [unroll] for (int dz = 0; dz <= 1; dz++) {
        [unroll] for (int dy = 0; dy <= 1; dy++) {
            [unroll] for (int dx = 0; dx <= 1; dx++) {
                int nx = ix + dx;
                int ny = iy + dy;
                int nz = iz + dz;
                if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z)
                    continue;

                float wx = (dx == 0) ? (1.0 - fx) : fx;
                float wy = (dy == 0) ? (1.0 - fy) : fy;
                float wz = (dz == 0) ? (1.0 - fz) : fz;
                float w = wx * wy * wz;
                // Global grid index (buffers are global-sized).
                uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z);

                // NOTE: plain += races when many particles splat into the
                // same cell. Slang's RWStructuredBuffer lacks a working
                // atomic-CAS on float elements (InterlockedCompareExchange
                // rejects buf[i] as non-l-value), so we accept the race —
                // density is sparse and collisions are rare in practice.
                ptcl_density[gi] += deposit * w;
                ptcl_vel_x[gi]   += vel.x * deposit * w;
                ptcl_vel_y[gi]   += vel.y * deposit * w;
                ptcl_vel_z[gi]   += vel.z * deposit * w;
                ptcl_color_r[gi] += color_deposit.r * w;
                ptcl_color_y[gi] += color_deposit.g * w;
                ptcl_color_b[gi] += color_deposit.b * w;
            }
        }
    }
}
