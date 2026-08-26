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
// Eq.15/16 scatter targets, bound as raw views: the accumulation must be a
// CAS atomicFloatAdd — plain += raced and drifted RYB over long loops.
RWByteAddressBuffer density : register(u0);
RWByteAddressBuffer color_r : register(u1);
RWByteAddressBuffer color_y : register(u2);
RWByteAddressBuffer color_b : register(u3);
RWStructuredBuffer<uint> ptcl_alive_out : register(u4);
// Wetness: depositing paint is WET paint. damp_dry only ever DECREASES
// wetness, so without a writer that sets it high on deposition, every painted
// cell reads as dried-solid (wetness < 0.01) the moment it lands — the fluid
// solve then treats fresh paint as an immovable wall and it never spreads.
// Set the receiving cells to fully wet here (the only place mass enters the
// grid).
RWByteAddressBuffer wetness : register(u5);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) { ptcl_alive_out[idx] = 0; return; }

    float3 pos = ptcl_pos[idx].xyz;
    float3 vel = ptcl_vel[idx].xyz;

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

    // Paper §5.2, LITERAL reading: transfer a particle back into the density
    // field "if the particle moves slowly and its distance to the bristles is
    // above D0". "Slowly" is the ABSOLUTE canvas-frame speed (the paper gives
    // no number; cb.slow_deposit_speed ≈ 2 cm/s in real units). The previous
    // relative-to-flow and landed-on-canvas readings were local inventions
    // from the era when the wake velocity did not decay; the wake now decays
    // through the paper's own §4.2 mechanisms (strong viscous Jacobi
    // diffusion homogenizing window momentum + dryness zeroing), so the
    // literal gate works: behind a moving brush the flow slows, particles
    // follow via the §4.3 FLIP/PIC update (γ=0.8 PIC-weighted), and the
    // trailing edge deposits continuously.
    // PEN-UP: the brush is away from the canvas — d_{B,k} against the stalled
    // sample positions no longer means proximity to a painting brush, so drop
    // the distance condition and let the whole carried swarm settle onto the
    // canvas as the residual flow decays.
    float speed = length(vel);
    bool near_brush = d_B < cb.D0 && cb.pen_down != 0;
    bool too_fast = speed > cb.slow_deposit_speed;
    if (near_brush || too_fast) {
        ptcl_alive_out[idx] = 1;
        return;
    }

    float4 col = ptcl_color[idx];  // RYB pigment + mass in w
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    // 2.5-cell kernel: wide enough that retiring-particle deposits merge
    // into a continuous band instead of a chain of one-cell clumps with
    // sub-threshold gaps between them (Σw=1 keeps mass exact).
    float h = cb.cell_size * 2.5;  // kernel radius (XY)
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

    // 5×5×5 neighborhood (±2 cells) with a 2.5-cell kernel, discretely
    // normalized: each particle deposits EXACTLY its mass m_k over the
    // neighborhood (Σ_c w_c = 1), so ρ_c += m_k · w_c keeps the particle mass
    // in DENSITY units — one emitted particle (m ≈ 0.05) adds ~0.05 to the
    // cells it covers. The wide footprint merges retiring-particle deposits
    // into a continuous band instead of a chain of clumps with
    // sub-threshold gaps between them. The previous 3×3×3 × (W·dV) form
    // deposited m·W·dV ≈ 4e-6 per cell at res 1024 (dV = cell³ ≈ 9.3e-10) —
    // four orders below the render threshold — so the trail rendered as
    // faint disconnected blobs. The dV factor only makes sense when m_k is a
    // physical mass and ρ a physical density; our m_k ("liquid load") and ρ
    // (normalized paint density) share one arbitrary unit scale, and the
    // discrete normalization keeps the grid↔particle round trip exactly
    // conservative on that scale (grid_to_particle's subtraction uses the
    // same normalized weights).
    float wsum = 0.0;
    [unroll] for (int dz = -2; dz <= 2; dz++) {
      [unroll] for (int dy = -2; dy <= 2; dy++) {
        [unroll] for (int dx = -2; dx <= 2; dx++) {
            int ix = cx + dx;
            int iy = cy + dy;
            int iz = cz + dz;
            if (ix < 0 || ix >= res || iy < 0 || iy >= res || iz < 0 || iz >= res_z) continue;
            float3 cell_pos = float3(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(iz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            wsum += W_smooth_3d(pos - cell_pos, h);
        }
      }
    }
    if (wsum < 1e-9) return;  // no valid neighbor cell (outside grid)

    [unroll] for (int dz = -2; dz <= 2; dz++) {
      [unroll] for (int dy = -2; dy <= 2; dy++) {
        [unroll] for (int dx = -2; dx <= 2; dx++) {
            int ix = cx + dx;
            int iy = cy + dy;
            int iz = cz + dz;
            if (ix < 0 || ix >= res || iy < 0 || iy >= res || iz < 0 || iz >= res_z) continue;

            float3 cell_pos = float3(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(iz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            float w = W_smooth_3d(pos - cell_pos, h) / wsum;

            // Global grid index (buffers are global-sized).
            uint gi = grid_idx_3d(uint(ix), uint(iy), uint(iz), res, res_z);

            // Eq.16 with discrete normalization: ρ_c += m_k · w_c. Color
            // accumulated as weighted RYB; brightness-preserving mixing is
            // deferred to the merge step. Atomic CAS adds make the scatter
            // exactly conservative (see common.slangh atomicFloatAdd).
            float w_new = col.w * w;
            atomicFloatAdd(density, gi * 4u, w_new);
            atomicFloatAdd(color_r, gi * 4u, col.x * w_new);
            atomicFloatAdd(color_y, gi * 4u, col.y * w_new);
            atomicFloatAdd(color_b, gi * 4u, col.z * w_new);
            // Freshly deposited paint is fully wet; drying (damp_dry) brings
            // it down over time. max() so re-depositing on an existing cell
            // keeps it wet (a re-stroke wets the paint again, as a real brush
            // would).
            atomicFloatMax(wetness, gi * 4u, 1.0);
        }
      }
    }

    // Kill the particle after transfer
    ptcl_alive_out[idx] = 0;
}
