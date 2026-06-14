// Bristle-Particle Liquid Transfer — Wetbrush Section 5.1
// Models per-sample liquid capacity M_j, absorption, emission, and color bleeding.
//
// Two passes:
//   Pass 0 (ABSORB): Load paint into bristle samples from supply (sample_color).
//     - Compute potential capacity M'_j from bristle density ψ (Eq.12)
//     - Compute actual capacity M_j from surface distance r_j (Eq.13)
//     - If sample is dry: seed with paint supply color
//     - If below capacity: absorb from supply, color_mix (Algorithm 2)
//     - Color bleeding: even saturated samples pick up new colors
//
//   Pass 1 (EMIT): Bristle samples → particles.
//     - If m_j > (1+ε) M_j: emit excess as particles in hemisphere pattern
//     - Particles share sample velocity (zero for now) and pigment
//
// Dispatch: Nb*S threads, 1 thread per bristle sample.

#include "common.slangh"

ConstantBuffer<BristleLiquidConstants> cb : register(b0);

// Bristle sample state (read-only in both passes)
StructuredBuffer<float4> sample_pos : register(t0);
StructuredBuffer<float4> sample_color : register(t1);    // RYB+density from resample

// Liquid state: ping-pong between two buffers
StructuredBuffer<SampleLiquid> sample_liquid_in : register(t2);

// Bristle rasterized density field ψ (for Eq.12 capacity)
StructuredBuffer<float> bristle_psi : register(t3);

// Grid density field (for Eq.13 surface distance estimation)
StructuredBuffer<float> grid_density : register(t4);

// Output: updated sample liquid state
RWStructuredBuffer<SampleLiquid> sample_liquid_out : register(u0);

// Output: particles (written in EMIT pass only)
// NOTE: float4 (not float3) to avoid RWStructuredBuffer<float3> stride hazards
// on some drivers; .xyz stores position/velocity, .w unused.
RWByteAddressBuffer ptcl_counter : register(u1);
RWStructuredBuffer<float4> ptcl_pos_out : register(u2);
RWStructuredBuffer<float4> ptcl_vel_out : register(u3);
RWStructuredBuffer<float4> ptcl_color_out : register(u4);
RWStructuredBuffer<uint> ptcl_alive_out : register(u5);

#ifndef PASS
#define PASS 0
#endif

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
        uint gi = grid_idx_3d(uint(ix), uint(iy), uint(iz), cb.grid_res, cb.grid_res_z);
        psi = bristle_psi[gi];
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

            uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), cb.grid_res, cb.grid_res_z);
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
    float has_supply = (s_color.w > 0.1) ? 1.0 : 0.0;

#if PASS == 0
    // ================================================================
    // ABSORB: Initialize/load paint into sample (Section 5.1)
    // ================================================================

    SampleLiquid result;
    result.pigment = c_j;
    result.mass = m_j;

    if (m_j < 0.01 && has_supply) {
        // Dry sample — seed with paint supply color
        result.mass = s_color.w * 0.5;
        result.pigment = s_color.rgb;
    } else if (m_j >= 0.01 && m_j < M_j && has_supply) {
        // Below capacity — absorb from continuous paint supply
        float supply = s_color.w * 0.05 * M_j;
        float new_mass = min(m_j + supply, M_j);

        // Color mixing with supply using brightness-preserving Algorithm 2
        result.pigment = color_mix(s_color.rgb, c_j, supply, m_j);
        result.mass = new_mass;
    } else if (m_j >= M_j && has_supply) {
        // Color bleeding (Section 5.1):
        // "a saturated bristle can still pick up new colors through
        //  its contact with paint liquid" — mix color without changing mass
        float bleed_weight = s_color.w * 0.02 * m_j;
        result.pigment = color_mix(s_color.rgb, c_j, bleed_weight, m_j);
        result.mass = min(m_j, M_j * (1.0 + cb.eps_emit));
    }

    // Clamp to capacity + epsilon
    result.mass = min(result.mass, M_j * (1.0 + cb.eps_emit));
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

    // Excess mass to emit
    float excess = m_j - emit_threshold_high;
    float per_particle = max(M_j * 0.15, 0.05);
    int num_emit = int(ceil(excess / per_particle));
    num_emit = min(num_emit, cb.max_emit_per_step);

    float total_emit_mass = 0.0;

    // Emit particles in a bottom hemisphere pattern (Section 5.1, Figure 10).
    // Bristles are planar (XY) at the canvas surface; emit upward into the
    // paint volume.
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;
    for (int ei = 0; ei < num_emit; ei++) {
        uint slot;
        ptcl_counter.InterlockedAdd(0, 1, slot);
        slot = slot % uint(cb.max_particles);

        // Uniform circular distribution in XY + upward arc in Z
        float angle = 6.2831853 * (float(ei) + 0.5) / float(num_emit);
        float jitter_r = cb.cell_size * 0.5;
        float z_lift = (float(ei) + 0.5) / float(num_emit) * cb.cell_size;
        float3 emit_pos = float3(
            s_pos.x + cos(angle) * jitter_r,
            s_pos.y + sin(angle) * jitter_r,
            canvas_z + z_lift);

        // Distribute excess evenly
        float emit_m = excess / float(num_emit);
        total_emit_mass += emit_m;

        ptcl_pos_out[slot] = float4(emit_pos, 0.0);
        ptcl_vel_out[slot] = float4(0.0, 0.0, 0.0, 0.0);
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
