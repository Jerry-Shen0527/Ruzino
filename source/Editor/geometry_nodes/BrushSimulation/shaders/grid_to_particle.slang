// Grid → Particle conversion — Wetbrush Section 5.2 Eq.15  (3D)
// Grid cells WITHIN D0 distance from bristles and with positive density
// are converted into particles via stratified sampling (3×3×3 = 27 candidates).
// After creating particles, REDUCES grid density by Eq.15:
//   ρ_c^new = max(ρ_c - m_k * Σ_k W(p_k - x_c, h), 0)
// Dispatched with res*res*res_z threads, one per 3D cell.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
StructuredBuffer<float> vel_x : register(t4);
StructuredBuffer<float> vel_y : register(t5);
StructuredBuffer<float> vel_z : register(t6);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float4> ptcl_pos : register(u1);
RWStructuredBuffer<float4> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);
// Density reduction output (Eq.15)
RWStructuredBuffer<float> density_out : register(u5);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int res = cb.grid_res;
    int res_z = cb.grid_res_z;
    int total = res * res * res_z;
    if (idx >= total) return;

    float cell_density = density[idx];
    if (cell_density < 0.15) {
        // No emission, propagate density unchanged
        density_out[idx] = cell_density;
        return;
    }

    int z = idx / (res * res);
    int rem = idx - z * res * res;
    int y = rem / res;
    int x = rem - y * res;

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_sz = cb.cell_size;
    float cell_cx = (float(x) + 0.5) * cell_sz - half_p + cb.grid_center_x;
    float cell_cy = (float(y) + 0.5) * cell_sz - half_p + cb.grid_center_y;
    float cell_cz = (float(z) + 0.5) * cell_sz - half_h + cb.grid_center_z;
    float3 cell_center = float3(cell_cx, cell_cy, cell_cz);

    // Only convert cells within D0 of the brush footprint (XY)
    float2 dxy = cell_center.xy - float2(cb.brush_pos_x, cb.brush_pos_y);
    float d_to_brush = length(dxy);
    if (d_to_brush > cb.D0) {
        density_out[idx] = cell_density;
        return;
    }

    // Stratified sampling: 27 candidate positions (3×3×3 sub-grid per cell)
    float h = cell_sz * 1.5;  // smoothing kernel radius

    // Particle mass per sub-cell: equal share of cell density
    float mass_per_candidate = cell_density / 27.0 * 0.5;  // 50% of density → particles

    int num_emitted = 0;
    float density_removed = 0.0;  // Σ(m_k · W(p_k - x_c, h)) for Eq.15

    [unroll] for (int sz = 0; sz < 3; sz++) {
      [unroll] for (int sy = 0; sy < 3; sy++) {
        [unroll] for (int sx = 0; sx < 3; sx++) {

            // Stratified sub-cell position (27 samples per cell, §5.2)

            float3 candidate = float3(
                cell_cx - cell_sz * 0.5 + (float(sx) + 0.5) / 3.0 * cell_sz,
                cell_cy - cell_sz * 0.5 + (float(sy) + 0.5) / 3.0 * cell_sz,
                cell_cz - cell_sz * 0.5 + (float(sz) + 0.5) / 3.0 * cell_sz);

            // Interpolate density at candidate from neighboring cells (3×3×3 kernel)
            float interp_density = 0.0;
            int cx = int(floor((candidate.x - cb.grid_center_x + half_p) / cell_sz));
            int cy = int(floor((candidate.y - cb.grid_center_y + half_p) / cell_sz));
            int cz = int(floor((candidate.z - cb.grid_center_z + half_h) / cell_sz));
            for (int dz = -1; dz <= 1; dz++) {
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                        if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z) continue;
                        float3 npos = float3(
                            (float(nx) + 0.5) * cell_sz - half_p + cb.grid_center_x,
                            (float(ny) + 0.5) * cell_sz - half_p + cb.grid_center_y,
                            (float(nz) + 0.5) * cell_sz - half_h + cb.grid_center_z);
                        float w = W_smooth_3d(candidate - npos, h);
                        uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z);
                        interp_density += density[gi] * w;
                    }
                }
            }

            if (interp_density < 0.05) continue;

            // Allocate particle slot (with wrap to prevent overflow)
            uint slot;
            ptcl_counter.InterlockedAdd(0, 1, slot);
            slot = slot % uint(cb.max_particles);

            float m_k = min(mass_per_candidate, 0.5);

            // Eq.15 density subtraction: accumulate Σ(m_k · W(p_k - x_c, h)).
            // (Must be the sum of per-particle mass×weight, NOT the product
            // of the two sums — that was the previous bug.)
            density_removed += m_k * W_smooth_3d(candidate - cell_center, h);
            num_emitted++;

            ptcl_pos[slot] = float4(candidate, 0.0);
            ptcl_vel[slot] = float4(vel_x[idx], vel_y[idx], vel_z[idx], 0.0);
            ptcl_color[slot] = float4(color_r[idx], color_y[idx], color_b[idx], m_k);
            ptcl_alive[slot] = 1;
        }
      }
    }

    // Eq.15: ρ_c^new = max(ρ_c - Σ_k(m_k · W(p_k - x_c, h)), 0)
    density_out[idx] = max(cell_density - density_removed, 0.0);
}
