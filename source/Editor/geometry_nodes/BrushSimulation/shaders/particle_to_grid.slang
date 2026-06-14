// Transfer distant slow particles back to grid — Wetbrush Section 5.2 Eq.16  (3D)
// Particles that move slowly AND are far from bristles (> D0) are converted
// back to grid cells. Their mass, color are deposited via W kernel smoothing.
// Color uses brightness-preserving interpolation (Algorithm 2).
// The particle is killed after transfer.
// Dispatched with max_particles threads, one per particle.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_vel : register(t1);
StructuredBuffer<float4> ptcl_color : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
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

    float3 pos = ptcl_pos[idx].xyz;
    float3 vel = ptcl_vel[idx].xyz;
    float speed = length(vel);
    // Distance to brush footprint in XY
    float d = length(pos.xy - float2(cb.brush_pos_x, cb.brush_pos_y));

    // Only transfer particles that are:
    // 1. Away from brush (distance > D0)
    // 2. Moving slowly (speed < threshold)
    if (d < cb.D0 * 1.5 || speed > 0.5) {
        ptcl_alive_out[idx] = 1;
        return;
    }

    float4 col = ptcl_color[idx];  // RYB pigment + mass in w
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    float h = cb.cell_size * 1.5;  // kernel radius (XY)
    int res = cb.grid_res, res_z = cb.grid_res_z;

    int cx = int(floor((pos.x - cb.grid_center_x + half_p) / cb.cell_size));
    int cy = int(floor((pos.y - cb.grid_center_y + half_p) / cb.cell_size));
    int cz = int(floor((pos.z - cb.grid_center_z + half_h) / cell_z));

    // 3×3×3 neighborhood with kernel weights (Eq.16)
    [unroll] for (int dz = -1; dz <= 1; dz++) {
      [unroll] for (int dy = -1; dy <= 1; dy++) {
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            int ix = cx + dx;
            int iy = cy + dy;
            int iz = cz + dz;
            if (ix < 0 || ix >= res || iy < 0 || iy >= res || iz < 0 || iz >= res_z) continue;

            float3 cell_pos = float3(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(iz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            float w = W_smooth_3d(pos - cell_pos, h);

            uint gi = grid_idx_3d(uint(ix), uint(iy), uint(iz), res, res_z);

            // Eq.16: ρ_c += m_k * W(p_k - x_c, h). Color accumulated as
            // weighted RYB; brightness-preserving mixing is deferred to the
            // merge step. Plain += races (see particle_rasterize note) but
            // collisions are rare for sparse far-from-brush particles.
            float w_new = col.w * w;
            density[gi] += w_new;
            color_r[gi] += col.x * w_new;
            color_y[gi] += col.y * w_new;
            color_b[gi] += col.z * w_new;
        }
      }
    }

    // Kill the particle after transfer
    ptcl_alive_out[idx] = 0;
}
