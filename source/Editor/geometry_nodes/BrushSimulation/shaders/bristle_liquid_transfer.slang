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

// Per-sample paint supply reservoir (consumed by ABSORB, §5.1 conservation).
// Read-modify-write: ABSORB takes min(available, capacity_room) and decrements.
RWStructuredBuffer<float> sample_supply : register(u6);

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
    float avail_supply = sample_supply[idx];  // finite reservoir (refilled/frame)

#if PASS == 0
    // ================================================================
    // ABSORB: Load paint into sample (Section 5.1, Eq.14).
    //
    // Color source priority (closest to paper semantics in the hybrid
    // representation): if the sample touches grid paint (grid-fluid surface),
    // absorb that real paint color — this is the Eq.14 "particle absorbed"
    // behavior mapped onto the density field. Otherwise fall back to the
    // supply reservoir (the brush's own loaded ink). Mass is conserved in
    // both cases: the supply is decremented; the grid-density subtraction
    // happens in the grid→particle/merge step.
    // ================================================================

    // Real paint near the sample in the grid (density-weighted RYB).
    float4 grid_paint = sample_grid_paint(s_pos);
    bool has_grid_paint = grid_paint.a > 0.05;

    // Absorb color: grid paint if present, else the brush supply color.
    float3 absorb_color = has_grid_paint ? grid_paint.rgb : s_color.rgb;
    // Absorb mass source: grid paint density drives absorption rate when
    // present (the brush is wiping through actual paint), capped by supply.
    float absorb_rate = has_grid_paint
        ? min(grid_paint.a, 1.0)   // rich paint nearby → fast pickup
        : (avail_supply * 0.05 * M_j);  // else draw from the dipped supply

    SampleLiquid result;
    result.pigment = c_j;
    result.mass = m_j;

    // Paper §5.1: "a brush bristle can keep paint liquid due to its
    // hydrophilicity." A dipped brush's samples are kept loaded by capillary
    // action from the ink supply — they saturate toward their capacity M_j
    // each frame, not at a slow fixed rate. The supply IS the paint source
    // for the initial stroke (the palette dip); once grid paint exists the
    // sample also absorbs from it (has_grid_paint path below). Without this
    // saturation the sample never reaches M_j, never overloads, and §5.1
    // emission never fires — the particle path produces zero paint (cold-start
    // deadlock). This matches the paper's implicit assumption that a loaded
    // brush carries paint in its bristles before the first stroke.
    if (m_j < 0.01 && (avail_supply > 0.01 || has_grid_paint)) {
        // Dry sample — seed/saturate from the paint source up to M_j.
        float take = min(M_j, avail_supply + grid_paint.a);
        result.mass = take;
        result.pigment = absorb_color;
        sample_supply[idx] = max(avail_supply - take, 0.0);
    } else if (m_j >= 0.01 && m_j < M_j && (avail_supply > 0.01 || has_grid_paint)) {
        // Below capacity — saturate up to M_j from the paint source (capillary
        // refill), capped by what the source can supply this frame.
        float room = M_j - m_j;
        float take = min(room, avail_supply + grid_paint.a);
        float new_mass = m_j + take;

        // Brightness-preserving color mix (Algorithm 2)
        result.pigment = color_mix(absorb_color, c_j, take, m_j);
        result.mass = new_mass;
        sample_supply[idx] = max(avail_supply - take, 0.0);
    } else if (m_j >= M_j && (avail_supply > 0.01 || has_grid_paint)) {
        // Saturated sample (paper §5.1: color bleeding + overload). A
        // hydrophilic bristle keeps taking up liquid past its static capacity
        // M_j when the supply is rich (a dipped brush). This overload is
        // exactly what §5.1 EMIT consumes (m_j > (1+ε)M_j → release
        // particles). The previous code clamped m_j to M_j*(1+ε) here, which
        // made overload impossible and EMIT never fired. Allow continued
        // uptake so the supply can push m_j into the overload region; EMIT
        // will then drain the excess back toward the threshold.
        float bleed_weight = (has_grid_paint ? grid_paint.a : avail_supply * 0.02) * m_j;
        result.pigment = color_mix(absorb_color, c_j, bleed_weight, m_j);
        // Keep absorbing mass from the supply past M_j (overload uptake).
        float take = min(avail_supply, cb.M_max);
        result.mass = m_j + take;
        sample_supply[idx] = max(avail_supply - take, 0.0);
    }

    // No clamp: overload (m_j > M_j) is the legitimate trigger for §5.1 EMIT.
    // EMIT drains the excess each frame, so m_j is bounded by the per-frame
    // supply uptake, not by an artificial cap here.
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

    // Sphere-cap radius for emission (same R_j as Eq.13).
    float R_j = pow(3.0 * M_prime / (4.0 * 3.14159265 * cb.rho_0), 1.0 / 3.0);
    float3 s_vel = sample_vel[idx].xyz;

    // Emit particles on the bottom hemisphere of radius R_j around the sample
    // (Section 5.1, Fig. 10). Each particle inherits the sample velocity.
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;
    for (int ei = 0; ei < num_emit; ei++) {
        uint slot;
        ptcl_counter.InterlockedAdd(0, 1, slot);
        slot = slot % uint(cb.max_particles);

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

        // Distribute excess evenly
        float emit_m = excess / float(num_emit);
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
