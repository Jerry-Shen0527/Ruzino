// Bristle-Particle Liquid Transfer — Wetbrush Section 5.1
// Models per-sample liquid capacity M_j, absorption, emission, and color bleeding.
//
// Two passes:
//   Pass 0 (ABSORB): color bleeding only — a (saturated) sample in contact
//     with grid paint picks up its hue (§5.1 "we still allow the color
//     mixing process to happen"). Mass does NOT flow here: the dip
//     initialization (m_j = M_max at stroke_start, deposit node) is the only
//     mass source, per the paper's initial-state treatment of a dipped
//     brush. (The paper's Eq.14 particle absorption is not implemented.)
//
//   Pass 1 (EMIT): Bristle samples → particles.
//     - If m_j > (1+ε) M_j: emit excess as particles in hemisphere pattern
//     - Particles share sample velocity and pigment
//
// Dispatch: Nb*S threads, 1 thread per bristle sample.

#include "common.slangh"

ConstantBuffer<BristleLiquidConstants> cb : register(b0);

// Bristle sample state (read-only in both passes)
StructuredBuffer<float4> sample_pos : register(t0);
StructuredBuffer<float4> sample_color : register(t1);    // RYB+density from resample
StructuredBuffer<float4> sample_vel : register(t5);      // sample velocity (for EMIT)

// Liquid state: ping-pong between two buffers
StructuredBuffer<SampleLiquid> sample_liquid_in : register(t2);

// Bristle rasterized density field ψ (for Eq.12 capacity)
StructuredBuffer<float> bristle_psi : register(t3);

// Grid density field (for Eq.13 surface distance estimation)
StructuredBuffer<float> grid_density : register(t4);

// Grid color fields (RYB) — ABSORB picks up color from grid paint near the
// sample (§5.1 Eq.14 in the hybrid representation: a sample near the
// grid-fluid surface absorbs the grid paint, rather than a fixed supply).
StructuredBuffer<float> grid_color_r : register(t6);
StructuredBuffer<float> grid_color_y : register(t7);
StructuredBuffer<float> grid_color_b : register(t8);

// Output: updated sample liquid state
RWStructuredBuffer<SampleLiquid> sample_liquid_out : register(u0);

// Output: particles (written in EMIT pass only)
// NOTE: float4 (not float3) to avoid RWStructuredBuffer<float3> stride hazards
// on some drivers; .xyz stores position/velocity, .w unused.
RWByteAddressBuffer ptcl_counter : register(u1);
// Global per-step birth budget (zeroed by the host each frame). EMIT reserves
// num_emit from it atomically; when spent, the sample KEEPS its load (no
// mass loss — it just tries again next step). See BristleLiquidConstants.
RWByteAddressBuffer emit_budget : register(u7);
RWStructuredBuffer<float4> ptcl_pos_out : register(u2);
RWStructuredBuffer<float4> ptcl_vel_out : register(u3);
RWStructuredBuffer<float4> ptcl_color_out : register(u4);
RWStructuredBuffer<uint> ptcl_alive_out : register(u5);

#ifndef PASS
#define PASS 0
#endif

// Per-particle emission mass floor (EMIT). Small particles at paper density:
// the drawable mass per sample (M_max − (1+ε)M_j ≈ 0.1) must spread over
// MANY steps×particles (cap 1/step), so each birth carries ~0.002 — with the
// old 0.05 floor, a sample's whole drawable mass left in 2 steps and the
// touchdown point swallowed half the dip (see max_emit_per_step note).
static const float PER_PARTICLE_FLOOR = 0.002;

// World position → 3D grid cell coordinates
float3 world_to_grid_f3(float3 pos)
{
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    return float3(
        (pos.x - cb.grid_center_x + half_p) / cb.cell_size,
        (pos.y - cb.grid_center_y + half_p) / cb.cell_size,
        (pos.z - cb.grid_center_z + half_h) / cell_z);
}

bool in_grid(int ix, int iy, int iz)
{
    return ix >= 0 && ix < cb.grid_res
        && iy >= 0 && iy < cb.grid_res
        && iz >= 0 && iz < cb.grid_res_z;
}

// Convert a GLOBAL grid cell (gx,gy,gz) to a WINDOW-LOCAL linear index into
// the window-sized bristle_density buffer (Group B). Paper §5.1: bristle
// samples only exist in the brush-local window, so any sample's nearest grid
// cell is inside [origin, origin+WIN). Returns false if the cell is outside
// the global grid OR outside the active window.
bool bristle_gi(int gx, int gy, int gz, out uint gi)
{
    if (!in_grid(gx, gy, gz)) {
        gi = 0u;
        return false;
    }
    int lx = gx - cb.window_origin_x;
    int ly = gy - cb.window_origin_y;
    int lz = gz - cb.window_origin_z;
    int WSX = cb.window_size_x;
    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX || lz < 0 || lz >= cb.grid_res_z) {
        gi = 0u;
        return false;
    }
    gi = uint(lz) * uint(WSX * WSX) + uint(ly) * uint(WSX) + uint(lx);
    return true;
}

// Convert a GLOBAL grid cell (gx,gy,gz) to a GLOBAL linear index into the
// full-grid main sim fields (Group A: density/color/vel/wetness). Returns
// false only if the cell is outside the global grid bounds.
bool grid_gi(int gx, int gy, int gz, out uint gi)
{
    if (!in_grid(gx, gy, gz)) {
        gi = 0u;
        return false;
    }
    gi = grid_idx_3d(uint(gx), uint(gy), uint(gz), cb.grid_res, cb.grid_res_z);
    return true;
}

// Sample the average grid paint color (RYB) and density near s_pos, over a
// small 3×3×3 neighborhood. Returns density-weighted RYB in .rgb and the
// total density in .a. Used by ABSORB to pick up real paint color from the
// grid-fluid surface (§5.1 Eq.14 in the hybrid representation).
float4 sample_grid_paint(float3 s_pos)
{
    float3 gp = world_to_grid_f3(s_pos);
    int cx = int(floor(gp.x));
    int cy = int(floor(gp.y));
    int cz = int(floor(gp.z));

    float3 color_sum = float3(0,0,0);
    float density_sum = 0.0;
    [unroll] for (int dz = -1; dz <= 1; dz++) {
      [unroll] for (int dy = -1; dy <= 1; dy++) {
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy, nz = cz + dz;
            if (!in_grid(nx, ny, nz)) continue;
            uint gi;
            if (!grid_gi(nx, ny, nz, gi)) continue;  // outside grid → empty
            float d = grid_density[gi];
            if (d > 0.01) {
                color_sum += float3(grid_color_r[gi], grid_color_y[gi], grid_color_b[gi]) * d;
                density_sum += d;
            }
        }
      }
    }
    float3 avg = (density_sum > 1e-6) ? color_sum / density_sum : float3(0,0,0);
    return float4(avg, density_sum);
}

// Compute potential capacity M'_j from bristle density ψ (Eq.12):
//   M'_j = max((1 - μ * ψ(x_j)) * M_max, M_min)
float compute_potential_capacity(float3 s_pos)
{
    float3 gp = world_to_grid_f3(s_pos);
    int ix = int(floor(gp.x));
    int iy = int(floor(gp.y));
    int iz = int(floor(gp.z));

    float psi = 0.0;
    if (in_grid(ix, iy, iz)) {
        uint gi;
        if (bristle_gi(ix, iy, iz, gi)) psi = bristle_psi[gi];
    }

    return max((1.0 - cb.mu * psi) * cb.M_max, cb.M_min);
}

// Estimate distance from sample to nearest grid-fluid OR solid (canvas)
// surface. Wetbrush §5.1: r_j is the distance "to such a surface" — both
// the grid-fluid surface and a solid boundary (canvas) count.
// Searches a 5×5×3 neighborhood for fluid cells and also measures the
// vertical distance to the canvas plane. Returns distance clamped to [0, R_j].
float estimate_surface_distance(float3 s_pos, float R_j)
{
    float3 gp = world_to_grid_f3(s_pos);
    int cx = int(floor(gp.x));
    int cy = int(floor(gp.y));
    int cz = int(floor(gp.z));

    float min_dist = R_j;  // default: no surface nearby

    // Grid-fluid surface: scan a small 3D neighborhood.
    [unroll]
    for (int dz = -1; dz <= 1; dz++) {
      [unroll]
      for (int dy = -2; dy <= 2; dy++) {
        [unroll]
        for (int dx = -2; dx <= 2; dx++) {
            int nx = cx + dx, ny = cy + dy, nz = cz + dz;
            if (!in_grid(nx, ny, nz)) continue;

            uint gi;
            if (!grid_gi(nx, ny, nz, gi)) continue;  // outside grid → empty
            if (grid_density[gi] > 0.1) {
                float cell_z = cb.height_extent / float(cb.grid_res_z);
                float3 cell_world = float3(
                    (float(nx) + 0.5) * cb.cell_size - cb.paper_size * 0.5 + cb.grid_center_x,
                    (float(ny) + 0.5) * cb.cell_size - cb.paper_size * 0.5 + cb.grid_center_y,
                    (float(nz) + 0.5) * cell_z - cb.height_extent * 0.5 + cb.grid_center_z);
                float dist = length(s_pos - cell_world);
                min_dist = min(min_dist, dist);
            }
        }
      }
    }

    // Solid (canvas) surface: the bottom plane of the paint volume.
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;
    float dist_canvas = abs(s_pos.z - canvas_z);
    min_dist = min(min_dist, dist_canvas);

    return clamp(min_dist, 0.0, R_j);
}

// Compute actual capacity M_j using spherical cap model (Eq.13):
//   R_j = cbrt(3 * M'_j / (4π * ρ₀))
//   M_j = M'_j - ρ₀ * π * (R_j - r_j)² * (R_j - (R_j - r_j)/3)
float compute_actual_capacity(float3 s_pos, float M_prime)
{
    // Spherical radius from capacity volume
    float R_j = pow(3.0 * M_prime / (4.0 * 3.14159265 * cb.rho_0), 1.0 / 3.0);

    // Distance to nearest surface, clamped to [0, R_j]
    float r_j = estimate_surface_distance(s_pos, R_j);

    // Spherical cap volume (Eq.13): ρ₀ * π * (R-r)² * (R - (R-r)/3)
    float dr = R_j - r_j;
    float cap = cb.rho_0 * 3.14159265 * dr * dr * (R_j - dr / 3.0);

    return max(M_prime - cap, cb.M_min * 0.5);
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int total_samples = cb.num_bristles * cb.samples_per_bristle;
    if (idx >= total_samples) return;

    float3 s_pos = sample_pos[idx].xyz;
    SampleLiquid liquid = sample_liquid_in[idx]; // {mass, pigment}
    float4 s_color = sample_color[idx];          // resampled RYB+density from paint supply

    // Compute capacity using Eq.12 → Eq.13
    float M_prime = compute_potential_capacity(s_pos);
    float M_j = compute_actual_capacity(s_pos, M_prime);

    float m_j = liquid.mass;
    float3 c_j = liquid.pigment;

#if PASS == 0
    // ================================================================
    // ABSORB — paper §5.1: MASS enters the samples only through the dip
    // initialization (stroke_start) and only through EMIT afterwards.
    // ================================================================

    // DIP (stroke_start frame): "dipped full" means saturated to the
    // capacity the bristle geometry ACTUALLY allows right now — Eq.12's
    // M'_j(ψ), crowding included. The earlier host-side m_j = M_max used
    // the UNCROWDED capacity, overloading interior samples (ψ high → real
    // capacity ≈ M_min) by ~30×: the whole brush body dripped continuously
    // (76800 births/step, pool flood). With m_j = M'_j, only samples whose
    // capacity DROPS from here (canvas contact / compression) overload and
    // emit — the paper's pressure-controls-output mechanism.
    if (cb.dip_frame != 0) {
        SampleLiquid dipped;
        dipped.mass = M_prime;
        dipped.pigment = s_color.rgb;  // ink color (resampled input color)
        sample_liquid_out[idx] = dipped;
        return;
    }

    // Real paint near the sample in the grid (density-weighted RYB). Color
    // source only — the grid keeps its mass.
    float4 grid_paint = sample_grid_paint(s_pos);
    bool has_grid_paint = grid_paint.a > 0.05;

    SampleLiquid result;
    result.pigment = c_j;
    result.mass = m_j;

    if (has_grid_paint) {
        // Color bleeding: contact weight from the local grid paint amount
        // (previously scaled by the supply reservoir, which no longer
        // exists — the bleed must survive the dip model change, or a wet
        // brush could never pick up canvas color).
        float bleed_weight = grid_paint.a * 0.05;
        if (bleed_weight + m_j > 1e-9) {
            result.pigment = color_mix(
                grid_paint.rgb, c_j, bleed_weight, m_j);
        }
    }
    sample_liquid_out[idx] = result;

#else // PASS == 1
    // ================================================================
    // EMIT: Sample → particles (Section 5.1)
    // When m_j > (1+ε)*M_j, emit particles in hemisphere pattern.
    // ================================================================

    float emit_threshold_high = (1.0 + cb.eps_emit) * M_j;

    if (m_j <= emit_threshold_high) {
        sample_liquid_out[idx] = liquid;
        return;
    }

    // Excess mass to emit. Paper §5.1 emission smoothing: one SMALL particle
    // per sample per step (max_emit_per_step=1, per_particle ≈ 0.002) paces a
    // saturated dip over a whole stroke instead of dumping it at touchdown.
    float excess = m_j - emit_threshold_high;
    float per_particle = max(M_j * 0.05, PER_PARTICLE_FLOOR);
    int num_emit = int(ceil(excess / per_particle));
    num_emit = min(num_emit, cb.max_emit_per_step);

    // Global birth budget: reserve num_emit atomically; a spent budget leaves
    // the load on the sample (mass stays put, emission retries next step).
    // Without this, the supply drip overloaded all 76800 samples every frame
    // and EMIT birthed ~76k particles/step, wrapping the pool.
    if (cb.emit_budget > 0) {
        // Probabilistic pre-gate BEFORE the atomic. The InterlockedAdd race
        // is won by whichever threads arrive first — in dispatch order, i.e.
        // the SAME first ~budget/max_emit samples (bristle #0's) every frame,
        // so all paint sprayed from one spot on the brush and the trail
        // rendered as a chain of clumps. Gate each sample with a
        // per-(sample, frame) random draw (brush position varies per frame)
        // so only ~budget/(Nb·S·max_emit) fraction compete, spread randomly
        // across the whole brush each frame.
        uint num_samples = uint(cb.num_bristles * cb.samples_per_bristle);
        float gate_p = min(
            1.0, float(cb.emit_budget) /
                     max(1.0, float(num_samples * uint(cb.max_emit_per_step))));
        float draw = frac(sin(
            cb.brush_pos_x * 12.9898 + cb.brush_pos_y * 78.233 +
            float(idx) * 0.6180339 + cb.brush_pos_z * 37.719) * 43758.5453);
        if (draw > gate_p) {
            sample_liquid_out[idx] = liquid;  // keep load, retry next step
            return;
        }
        uint used;
        emit_budget.InterlockedAdd(0, uint(num_emit), used);
        if (used >= uint(cb.emit_budget)) {
            sample_liquid_out[idx] = liquid;  // keep load, no emission
            return;
        }
        num_emit = int(min(
            uint(num_emit), uint(cb.emit_budget) - used));
    }

    float total_emit_mass = 0.0;

    // Sphere-cap radius for emission (same R_j as Eq.13).
    float R_j = pow(3.0 * M_prime / (4.0 * 3.14159265 * cb.rho_0), 1.0 / 3.0);
    float3 s_vel = sample_vel[idx].xyz;

    // Emit particles on the bottom hemisphere of radius R_j around the sample
    // (Section 5.1, Fig. 10). Each particle inherits the sample velocity.
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;
    for (int ei = 0; ei < num_emit; ei++) {
        uint slot;
        ptcl_counter.InterlockedAdd(0, 1, slot);
        // Pool exhausted: keep this birth's mass on the sample (retry next
        // step). The old `slot % max` wrap overwrote a LIVE particle and
        // silently destroyed its mass.
        if (slot >= uint(cb.max_particles)) break;

        // Random direction on the lower hemisphere (z <= 0 relative to sample).
        uint seed = uint(idx) * 1973u + uint(ei) * 9277u;
        float r1 = frac(sin(float(seed)) * 43758.5453);
        float r2 = frac(sin(float(seed + 1u)) * 43758.5453);
        float phi = 6.2831853 * r1;            // azimuth
        float cos_theta = 1.0 - 2.0 * r2;      // lower hemisphere: cos in [-1, 0]
        cos_theta = -abs(cos_theta);           // force z <= 0 (toward canvas)
        float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));

        float3 dir = float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
        float3 emit_pos = s_pos + dir * R_j;
        // Keep emission inside the paint volume (above the canvas).
        emit_pos.z = max(emit_pos.z, canvas_z);

        // Distribute: each birth carries per_particle at most. When the
        // per-step cap binds (num_emit < ceil(excess/per_particle)), the
        // un-capped excess/num_emit would load ONE particle with the whole
        // excess — clamp so the sample keeps the remainder for later steps.
        float emit_m = min(excess / float(num_emit), per_particle);
        total_emit_mass += emit_m;

        ptcl_pos_out[slot] = float4(emit_pos, 0.0);
        ptcl_vel_out[slot] = float4(s_vel, 0.0);   // inherit sample velocity
        ptcl_color_out[slot] = float4(c_j, emit_m);
        ptcl_alive_out[slot] = 1;
    }

    // Reduce sample load by emitted amount
    SampleLiquid result;
    result.mass = m_j - total_emit_mass;
    result.pigment = c_j;
    sample_liquid_out[idx] = result;
#endif
}
