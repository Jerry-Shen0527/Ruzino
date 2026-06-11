// Transfer distant slow particles back to grid — Wetbrush Section 5.2 Eq.16
// Particles that move slowly AND are far from bristles (> D0) are converted
// back to grid cells. Their mass, color, are deposited via W kernel smoothing.
// Color uses brightness-preserving interpolation (Algorithm 2).
// The particle is killed after transfer.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float2> ptcl_pos : register(t0);
StructuredBuffer<float2> ptcl_vel : register(t1);
StructuredBuffer<float4> ptcl_color : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
// Grid state for brightness-preserving color merge
StructuredBuffer<float> grid_density_in : register(t4);
StructuredBuffer<float> grid_color_r_in : register(t5);
StructuredBuffer<float> grid_color_y_in : register(t6);
StructuredBuffer<float> grid_color_b_in : register(t7);
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
    float2 vel = ptcl_vel[idx];
    float speed = length(vel);
    float d = length(pos - float2(cb.brush_pos_x, cb.brush_pos_y));

    // Only transfer particles that are:
    // 1. Away from brush (distance > D0)
    // 2. Moving slowly (speed < threshold)
    if (d < cb.D0 * 1.5 || speed > 0.5) {
        ptcl_alive_out[idx] = 1;
        return;
    }

    float4 col = ptcl_color[idx];  // RYB pigment + mass in w
    float half_p = cb.paper_size * 0.5;
    float h = cb.cell_size * 1.5;  // kernel radius

    // Deposit to surrounding cells using W kernel (Eq.16)
    int cx = int(floor((pos.x - cb.grid_center_x + half_p) / cb.cell_size));
    int cy = int(floor((pos.y - cb.grid_center_y + half_p) / cb.cell_size));

    // 3×3 neighborhood with kernel weights
    [unroll]
    for (int dy = -1; dy <= 1; dy++) {
        [unroll]
        for (int dx = -1; dx <= 1; dx++) {
            int ix = cx + dx;
            int iy = cy + dy;
            if (ix < 0 || ix >= cb.grid_res || iy < 0 || iy >= cb.grid_res) continue;

            float2 cell_pos = float2(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y);
            float2 r = pos - cell_pos;
            float w = W_smooth(r, h);

            uint gi = grid_idx(uint(ix), uint(iy), cb.grid_res);

            // Density deposit (Eq.16): ρ_c += m_k * W(p_k - x_c, h)
            density[gi] += col.w * w;

            // Color deposit with brightness-preserving merge (Algorithm 2)
            // Weight-by-density linear interpolation in RYB space,
            // then brightness-corrected using Algorithm 2.
            float w_new = col.w * w;
            float w_old = density[gi];
            float3 existing_ryb = float3(
                max(grid_color_r_in[gi], 0.0),
                max(grid_color_y_in[gi], 0.0),
                max(grid_color_b_in[gi], 0.0));

            // Total weight after this deposit
            float w_total = w_old + w_new;
            if (w_total > 0.01) {
                // Color mix (Algorithm 2): weighted blend in RYB with brightness correction
                float3 mixed = color_mix(col.rgb, existing_ryb, w_new, w_old);
                // Write back the normalized color (stored as density-weighted)
                color_r[gi] = mixed.r * w_total;
                color_y[gi] = mixed.g * w_total;
                color_b[gi] = mixed.b * w_total;
            } else {
                // Negligible existing color — direct deposit
                color_r[gi] += col.x * w_new;
                color_y[gi] += col.y * w_new;
                color_b[gi] += col.z * w_new;
            }
        }
    }

    // Kill the particle after transfer
    ptcl_alive_out[idx] = 0;
}
