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
// Bristle sample positions for the d_{B,k} distance query (paper §5.2 Table 1:
// d_{B,k} is the particle distance TO BRISTLES, not to the brush center).
StructuredBuffer<float4> sample_pos : register(t4);
RWStructuredBuffer<float> density : register(u0);
RWStructuredBuffer<float> color_r : register(u1);
RWStructuredBuffer<float> color_y : register(u2);
RWStructuredBuffer<float> color_b : register(u3);
RWStructuredBuffer<uint> ptcl_alive_out : register(u4);
// Wetness: depositing paint is WET paint. damp_dry only ever DECREASES
// wetness, so without a writer that sets it high on deposition, every painted
// cell reads as dried-solid (wetness < 0.01) the moment it lands — the fluid
// solve then treats fresh paint as an immovable wall and it never spreads.
// Set the receiving cells to fully wet here (the only place mass enters the
// grid).
RWStructuredBuffer<float> wetness : register(u5);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) { ptcl_alive_out[idx] = 0; return; }

    float3 pos = ptcl_pos[idx].xyz;
    float3 vel = ptcl_vel[idx].xyz;
    float speed = length(vel);

    // Paper §5.2 / Table 1: the grid↔particle conversion boundary is d_{B,k},
    // the particle's distance TO BRISTLES (not to the brush footprint center).
    // A particle within D0 of any bristle sample stays a particle; one beyond
    // D0 of every sample is grid-liquid and deposits here. Using the brush
    // CENTER distance (the old code, d >= D0*1.5) locked particles to the
    // brush footprint even after the brush moved away, so deposition never
    // fired. With d_{B,k}, a particle whose nearest bristle has moved more
    // than D0 away (brush departed) deposits immediately.
    int num_samples = cb.num_bristles * cb.samples_per_bristle;
    float best_d2 = 1e30;
    for (int s = 0; s < num_samples; s++) {
        float3 d = pos - sample_pos[s].xyz;
        float d2 = dot(d, d);
        if (d2 < best_d2) best_d2 = d2;
    }
    float d_B = sqrt(best_d2);

    // Only transfer particles that are:
    // 1. Away from bristles (d_{B,k} >= D0)
    // 2. Moving slowly (speed < threshold) — paper §5.2 "moves slowly"
    if (d_B < cb.D0 || speed > 0.5) {
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

    // Deposit anywhere in the GLOBAL grid. The previous "keep alive if outside
    // the active window" check (for conservation symmetry with grid_to_particle)
    // stranded every particle the brush left behind: the brush window is ~125
    // cells wide but the stroke is ~300 cells long, so after a few frames
    // nearly all emitted particles were outside the window, never depositing,
    // and the pool saturated at 262k with ~0 grid density. Paint left behind
    // by a moving brush SHOULD deposit onto the grid and stay — that is the
    // persistent canvas. Only deposit into valid global-grid cells (the bounds
    // check in the neighborhood loop below handles out-of-grid).

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

            // Global grid index (buffers are global-sized).
            uint gi = grid_idx_3d(uint(ix), uint(iy), uint(iz), res, res_z);

            // Eq.16: ρ_c += m_k * W(p_k - x_c, h) * dV. W is a normalized SPH
            // kernel (per unit volume); multiply by the cell volume dV to get
            // the cell-density contribution. Without dV the kernel value at
            // r≈0 is ~1/(h³·(2π)^1.5) ≈ 317000 for cell-size h, causing
            // runaway density. Color accumulated as weighted RYB; brightness-
            // preserving mixing is deferred to the merge step. Plain += races
            // (see particle_rasterize note) but collisions are rare for sparse
            // far-from-brush particles.
            float dV = cb.cell_size * cb.cell_size * cell_z;
            float w_new = col.w * w * dV;
            density[gi] += w_new;
            color_r[gi] += col.x * w_new;
            color_y[gi] += col.y * w_new;
            color_b[gi] += col.z * w_new;
            // Freshly deposited paint is fully wet; drying (damp_dry) brings
            // it down over time. max() so re-depositing on an existing cell
            // keeps it wet (a re-stroke wets the paint again, as a real brush
            // would).
            wetness[gi] = max(wetness[gi], 1.0f);
        }
      }
    }

    // Kill the particle after transfer
    ptcl_alive_out[idx] = 0;
}
