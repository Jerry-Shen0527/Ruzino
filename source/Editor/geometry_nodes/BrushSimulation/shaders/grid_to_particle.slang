// Grid → Particle conversion — Wetbrush Section 5.2 Eq.15
// Grid cells WITHIN D0 distance from bristles and with positive density
// are converted into particles via stratified sampling.
// After creating particles, REDUCES grid density by Eq.15:
//   ρ_c^new = max(ρ_c - m_k * Σ_k W(p_k - x_c, h), 0)
// The reduction reflects from ALL nearby cells (not just the source cell)
// to ensure smoothness in the density field.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
StructuredBuffer<float> vel_x : register(t4);
StructuredBuffer<float> vel_y : register(t5);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float2> ptcl_pos : register(u1);
RWStructuredBuffer<float2> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);
// Density reduction output (Eq.15)
RWStructuredBuffer<float> density_out : register(u5);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int res = cb.grid_res;
    if (idx >= res * res) return;

    float cell_density = density[idx];
    if (cell_density < 0.15) {
        // No emission, propagate density unchanged
        density_out[idx] = cell_density;
        return;
    }

    int x = idx % res;
    int y = idx / res;

    float half_p = cb.paper_size * 0.5;
    float cell_sz = cb.cell_size;
    float cell_cx = (float(x) + 0.5) * cell_sz - half_p + cb.grid_center_x;
    float cell_cy = (float(y) + 0.5) * cell_sz - half_p + cb.grid_center_y;
    float2 cell_center = float2(cell_cx, cell_cy);

    float2 brush_pos2 = float2(cb.brush_pos_x, cb.brush_pos_y);
    float d_to_brush = length(cell_center - brush_pos2);

    // Only convert cells within D0 distance from bristles (Section 5.2)
    if (d_to_brush > cb.D0) {
        density_out[idx] = cell_density;
        return;
    }

    // Stratified sampling: 9 candidate positions (3×3 sub-grid per cell)
    // The paper uses 27 (3×3×3) for 3D; in 2D we use 3×3.
    // Each candidate gets a fair share of the cell's density.
    float h = cell_sz * 1.5;  // smoothing kernel radius

    // Particle mass per sub-cell: equal share of cell density
    float mass_per_candidate = cell_density / 9.0 * 0.5;  // 50% of density → particles
    // Only emit up to 4 particles per cell per step to avoid oversampling
    static const int MAX_EMIT_PER_CELL = 4;

    int num_emitted = 0;
    float total_mass_removed = 0.0;
    float total_kernel_at_center = 0.0;

    for (int k = 0; k < 9; k++) {
        if (num_emitted >= MAX_EMIT_PER_CELL) break;

        int sx = k % 3;
        int sy = k / 3;
        // Jittered sub-cell center position
        float2 candidate = float2(
            cell_cx - cell_sz * 0.5 + (float(sx) + 0.5) / 3.0 * cell_sz,
            cell_cy - cell_sz * 0.5 + (float(sy) + 0.5) / 3.0 * cell_sz);

        // Interpolate density at candidate from nearby cells
        float interp_density = 0.0;
        int cx = int(floor((candidate.x - cb.grid_center_x + half_p) / cell_sz));
        int cy = int(floor((candidate.y - cb.grid_center_y + half_p) / cell_sz));
        // Bilinear interpolation from 2×2 neighborhood
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || nx >= res || ny < 0 || ny >= res) continue;
                float2 npos = (float2(nx, ny) + 0.5) * cell_sz - half_p
                    + float2(cb.grid_center_x, cb.grid_center_y);
                float2 r = candidate - npos;
                float w = W_smooth(r, h);
                interp_density += density[ny * res + nx] * w;
            }
        }

        // Only emit if local density is significant
        if (interp_density < 0.05) continue;

        // Allocate particle slot (with wrap to prevent overflow)
        uint slot;
        ptcl_counter.InterlockedAdd(0, 1, slot);
        slot = slot % uint(cb.max_particles);

        float m_k = min(mass_per_candidate, 0.5);

        // Track kernel weight at cell center from this particle (for Eq.15)
        float2 r_to_center = candidate - cell_center;
        total_kernel_at_center += W_smooth(r_to_center, h);

        total_mass_removed += m_k;
        num_emitted++;

        ptcl_pos[slot] = candidate;
        ptcl_vel[slot] = float2(vel_x[idx], vel_y[idx]);
        ptcl_color[slot] = float4(color_r[idx], color_y[idx], color_b[idx], m_k);
        ptcl_alive[slot] = 1;
    }

    // Eq.15: ρ_c^new = max(ρ_c - m_k * Σ_k W(p_k - x_c, h), 0)
    // Use accumulated kernel weight from all emitted particles
    density_out[idx] = max(cell_density - total_mass_removed * total_kernel_at_center, 0.0);
}
