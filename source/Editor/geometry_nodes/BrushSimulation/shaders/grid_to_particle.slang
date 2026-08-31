// Grid → Particle conversion — Wetbrush Section 5.2 Eq.15  (3D)
// Grid cells WITHIN D0 distance from bristles and with positive density
// are converted into particles via stratified sampling (3×3×3 = 27 candidates).
// After creating particles, REDUCES grid density by Eq.15:
//   ρ_c^new = max(ρ_c - m_k * Σ_k W(p_k - x_c, h), 0)
// Paper §5.2: "c can be any cell near new particles and it does not have
// to emit any particle ... to ensure sufficient smoothness in the density
// field." So each emitted particle subtracts its mass (weighted by W) from
// its 3×3×3 neighborhood of cells, NOT just the emitting cell. The host
// seeds density_out with the current density before this dispatch, so the
// shader only needs to subtract contributions — atomically: the Eq.15
// subtraction goes through atomicFloatAdd on RWByteAddressBuffer views
// (see common.slangh), so the grid↔particle round trip is exactly
// conservative.
// Dispatched over the active window (win_n3d threads). Buffer indexing is
// GLOBAL (buffers are global-sized).

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
StructuredBuffer<float> vel_x : register(t4);
StructuredBuffer<float> vel_y : register(t5);
StructuredBuffer<float> vel_z : register(t6);
// §4.2 dryness: "once the dryness of a cell reaches a threshold, we ignore
// its velocity and we treat it as a solid cell in pressure projection."
// DRIED PAINT IS SOLID — the §5.2 drain converts LIQUID to particles and
// must not resurrect solid paint. Without this gate the drain disc
// (brush_radius + D0 ≈ 1.5 cm at the 1u=1cm calibration) re-liquefied the
// dried trail every frame: each drain→redeposit round trip jitter-walks the
// mass one cell, so the trail edge random-walked below the render threshold
// (paint "vanishing then reappearing", sawtooth scales after the window
// scroll). Wetness < 0.01 = the SAME solid threshold as is_solid in
// fluid_jacobi / damp_dry.
StructuredBuffer<float> wetness : register(t7);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float4> ptcl_pos : register(u1);
RWStructuredBuffer<float4> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);
// Density reduction output (Eq.15). Seeded with current density by the host;
// this shader subtracts particle contributions via the 3×3×3 neighborhood.
RWByteAddressBuffer density_out : register(u5);
// Color reduction outputs. Seeded with the current color_r/y/b by the host.
// When a cell loses density to a new particle (Eq.15), it must lose the SAME
// fraction of its premultiplied color, otherwise color/density drifts: the
// grid keeps color while density walks out as particles, and the particle
// (which reads the cell's absolute color) carries a mis-scaled pigment. This
// is the §14 TODO #2 fix — color transfers WITH mass, conserving the ratio.
// Raw views + atomicFloatAdd(-loss): the Eq.15 subtraction must be a CAS —
// plain -= raced on the RWStructuredBuffer and drifted RYB over long loops.
RWByteAddressBuffer color_r_out : register(u6);
RWByteAddressBuffer color_y_out : register(u7);
RWByteAddressBuffer color_b_out : register(u8);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int res = cb.grid_res;
    int res_z = cb.grid_res_z;
    // Dispatch over the active window. Map tid → window-local (lx,ly,lz),
    // then to global (x,y,z) via window_origin for world coords + global index.
    int WSX = cb.window_size_x, WSZ = cb.window_size_z;
    int total = WSX * WSX * WSZ;
    if (idx >= total) return;

    int lz = idx / (WSX * WSX);
    int rem = idx - lz * WSX * WSX;
    int ly = rem / WSX;
    int lx = rem - ly * WSX;
    int x = lx + cb.window_origin_x;
    int y = ly + cb.window_origin_y;
    int z = lz + cb.window_origin_z;
    if (x < 0 || x >= res || y < 0 || y >= res || z < 0 || z >= res_z) return;

    // Global buffer index.
    uint gidx = grid_idx_3d(uint(x), uint(y), uint(z), res, res_z);
    float cell_density = density[gidx];
    if (cell_density < 0.05) return;
    // Dried paint is solid (§4.2) — never drained (see wetness decl above).
    if (wetness[gidx] < 0.01) return;

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_sz = cb.cell_size;                 // XY cell size
    float cell_z  = cb.height_extent / float(res_z);  // Z cell size (≠ XY)
    float cell_cx = (float(x) + 0.5) * cell_sz - half_p + cb.grid_center_x;
    float cell_cy = (float(y) + 0.5) * cell_sz - half_p + cb.grid_center_y;
    float cell_cz = (float(z) + 0.5) * cell_z - half_h + cb.grid_center_z;
    float3 cell_center = float3(cell_cx, cell_cy, cell_cz);

    // PEN-UP: no conversion — a lifted brush has no particle zone around it
    // (§5.2's "liquid within D0 of bristles must be particles" only applies
    // while the brush is painting). Suspending this also stops the
    // convert→re-deposit churn from running against a parked brush.
    if (cb.pen_down == 0) return;

    // Paper §5.2: "liquid within distance D0 from bristles must be represented
    // by particles" — convert every cell within D0 of the bristles. The exact
    // test is a distance to the nearest SAMPLE (samples spread across the
    // footprint out to the brush radius), so the center-distance bound is
    // brush_radius + D0: a cell within that radius of the center is within
    // D0 of some sample. The drained zone renders via the raster composite
    // (pack_float4); trail continuity behind the brush comes from particles
    // crossing D0 and depositing (Eq.16).
    float2 dxy = cell_center.xy - float2(cb.brush_pos_x, cb.brush_pos_y);
    float d_to_brush = length(dxy);
    if (d_to_brush > cb.brush_radius + cb.D0) return;

    // Stratified sampling: 27 candidate positions (3×3×3 sub-grid per cell)
    // Same WORLD-absolute kernel radius as particle_to_grid (0.0244 cm ≈ a
    // bristle thickness; = 2.5 cells at the design grid) — the drain stays
    // inside the adhesion bulb while deposits feather outward, so each
    // convert→re-deposit cycle smooths the trail instead of notching it.
    float h = 0.0244f;  // smoothing kernel radius (cm, XY-isotropic approx)

    // Particle mass per sub-cell: equal share of cell density
    float mass_per_candidate = cell_density / 27.0;

    // Normalized pigment of THIS cell (RYB per unit density). Particles
    // inherit this so their color is a proper 0-1 RYB vector, not the
    // premultiplied absolute color stored in the grid. particle_to_grid
    // multiplies it back by mass on deposit, keeping color conserved.
    float inv_d = 1.0 / max(cell_density, 1e-8f);
    float3 norm_ryb = float3(color_r[gidx], color_y[gidx], color_b[gidx]) * inv_d;

    [unroll] for (int sz = 0; sz < 3; sz++) {
      [unroll] for (int sy = 0; sy < 3; sy++) {
        [unroll] for (int sx = 0; sx < 3; sx++) {

            // Stratified sub-cell position (27 samples per cell, §5.2) with a
            // random jitter INSIDE each stratum. Fixed stratum centers put
            // every converted particle on a global 3×3×3 sub-lattice — the
            // debug particle render exposed the paint mass as a regular grid
            // instead of free particles. Stratified sampling in the Monte
            // Carlo sense IS jittered strata (and re-deposit then no longer
            // cancels systematically onto the same grid points). Same
            // frac(sin) hash as the bristle EMIT pass, seeded per cell +
            // stratum so every candidate gets a stable, distinct offset.
            float3 stratum = float3(cell_sz / 3.0, cell_sz / 3.0, cell_z / 3.0);
            uint jseed = uint(x) * 73856093u ^ uint(y) * 19349663u ^
                         uint(z) * 83492791u ^ uint(sx) * 2971u ^
                         uint(sy) * 4177u ^ uint(sz) * 7537u;
            float jx = frac(sin(float(jseed)) * 43758.5453) - 0.5;
            float jy = frac(sin(float(jseed + 1u)) * 43758.5453) - 0.5;
            float jz = frac(sin(float(jseed + 2u)) * 43758.5453) - 0.5;

            float3 candidate = float3(
                cell_cx - cell_sz * 0.5 + (float(sx) + 0.5) / 3.0 * cell_sz,
                cell_cy - cell_sz * 0.5 + (float(sy) + 0.5) / 3.0 * cell_sz,
                cell_cz - cell_z  * 0.5 + (float(sz) + 0.5) / 3.0 * cell_z)
                + float3(jx, jy, jz) * stratum;

            // Interpolate density at candidate from neighboring cells (3×3×3
            // kernel), discretely normalized (Σw = 1) so the interpolated
            // value is in the same DENSITY units as the cells themselves and
            // the 0.02 gate below reads on that scale. The previous W·dV
            // form yielded ρ·dV ≈ 1e-4 — the gate never fired at res ≥1024.
            float interp_density = 0.0;
            int cx = int(floor((candidate.x - cb.grid_center_x + half_p) / cell_sz));
            int cy = int(floor((candidate.y - cb.grid_center_y + half_p) / cell_sz));
            int cz = int(floor((candidate.z - cb.grid_center_z + half_h) / cell_z));
            float interp_wsum = 0.0;
            for (int dz = -1; dz <= 1; dz++) {
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                        if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z) continue;
                        // Only LIQUID density can convert (dry neighbors are
                        // solid — excluded from the normalization too, so the
                        // interpolated value stays a pure liquid density).
                        if (wetness[grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z)] < 0.01f) continue;
                        float3 npos = float3(
                            (float(nx) + 0.5) * cell_sz - half_p + cb.grid_center_x,
                            (float(ny) + 0.5) * cell_sz - half_p + cb.grid_center_y,
                            (float(nz) + 0.5) * cell_z  - half_h + cb.grid_center_z);
                        interp_wsum += W_smooth_3d(candidate - npos, h);
                    }
                }
            }
            if (interp_wsum < 1e-9) continue;
            for (int dz = -1; dz <= 1; dz++) {
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                        if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z) continue;
                        if (wetness[grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z)] < 0.01f) continue;
                        float3 npos = float3(
                            (float(nx) + 0.5) * cell_sz - half_p + cb.grid_center_x,
                            (float(ny) + 0.5) * cell_sz - half_p + cb.grid_center_y,
                            (float(nz) + 0.5) * cell_z  - half_h + cb.grid_center_z);
                        float w = W_smooth_3d(candidate - npos, h) / interp_wsum;
                        uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z);
                        interp_density += density[gi] * w;
                    }
                }
            }

            if (interp_density < 0.02) continue;

            // Allocate particle slot; on pool exhaustion the candidate is
            // skipped and the cell keeps its density (no wrap-around: the
            // old `slot % max` overwrote a LIVE particle, silently
            // destroying its mass).
            uint slot;
            ptcl_counter.InterlockedAdd(0, 1, slot);
            if (slot >= uint(cb.max_particles)) continue;

            float m_k = mass_per_candidate;

            // Eq.15 density subtraction spread to the particle's 3×3×3
            // neighborhood (paper §5.2: "c can be any cell near new particles
            // and it does not have to emit any particle"). Each neighbor cell
            // c loses m_k · w_c with the SAME discrete normalization
            // (Σw = 1) that particle_to_grid deposits with, so the 27
            // candidates × (ρ_c/27) round trip removes and restores exactly
            // ρ_c — conservative on the shared density/mass unit scale. Plain
            // -= races but collisions are rare (see common.slangh note).
            //
            // Color subtraction (§14 TODO #2): each neighbor loses the SAME
            // fraction of its premultiplied color, so its color/density ratio
            // is preserved. Without this the grid kept color while density
            // left as particles, diluting the rendered paint toward black.
            int pcx = int(floor((candidate.x - cb.grid_center_x + half_p) / cell_sz));
            int pcy = int(floor((candidate.y - cb.grid_center_y + half_p) / cell_sz));
            int pcz = int(floor((candidate.z - cb.grid_center_z + half_h) / cell_z));
            float sub_wsum = 0.0;
            for (int sdt = -1; sdt <= 1; sdt++) {
              for (int sdy = -1; sdy <= 1; sdy++) {
                for (int sdx = -1; sdx <= 1; sdx++) {
                    int nx = pcx + sdx, ny = pcy + sdy, nz = pcz + sdt;
                    if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z) continue;
                    // Never subtract from DRY (solid) neighbors — solid paint
                    // cannot be carried by a liquid particle (§4.2 dryness).
                    if (wetness[grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z)] < 0.01f) continue;
                    float3 npos = float3(
                        (float(nx) + 0.5) * cell_sz - half_p + cb.grid_center_x,
                        (float(ny) + 0.5) * cell_sz - half_p + cb.grid_center_y,
                        (float(nz) + 0.5) * cell_z  - half_h + cb.grid_center_z);
                    sub_wsum += W_smooth_3d(candidate - npos, h);
                }
              }
            }
            if (sub_wsum < 1e-9) continue;
            for (int sdt = -1; sdt <= 1; sdt++) {
              for (int sdy = -1; sdy <= 1; sdy++) {
                for (int sdx = -1; sdx <= 1; sdx++) {
                    int nx = pcx + sdx, ny = pcy + sdy, nz = pcz + sdt;
                    if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z) continue;
                    if (wetness[grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z)] < 0.01f) continue;
                    float3 npos = float3(
                        (float(nx) + 0.5) * cell_sz - half_p + cb.grid_center_x,
                        (float(ny) + 0.5) * cell_sz - half_p + cb.grid_center_y,
                        (float(nz) + 0.5) * cell_z  - half_h + cb.grid_center_z);
                    float w = W_smooth_3d(candidate - npos, h) / sub_wsum;
                    uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z);
                    // density_out / color_*_out are WINDOW-SIZED tmps (the
                    // host seeds them from the global canvas fields and copies
                    // back). The D0 drain zone + kernel apron sits well inside
                    // the 128-cell window; out-of-window writes are skipped.
                    int wo = win_field_idx(
                        nx, ny, nz, cb.window_origin_x, cb.window_origin_y,
                        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
                    if (wo < 0) continue;
                    float loss = m_k * w;
                    atomicFloatAdd(density_out, uint(wo) * 4u, -loss);
                    // Neighbor pigment (normalized); fall back to this cell's
                    // pigment if the neighbor has no density to normalize by.
                    float nd = density[gi];
                    float3 n_ryb = (nd > 1e-6f)
                        ? float3(color_r[gi], color_y[gi], color_b[gi]) / nd
                        : norm_ryb;
                    atomicFloatAdd(color_r_out, uint(wo) * 4u, -n_ryb.x * loss);
                    atomicFloatAdd(color_y_out, uint(wo) * 4u, -n_ryb.y * loss);
                    atomicFloatAdd(color_b_out, uint(wo) * 4u, -n_ryb.z * loss);
                }
              }
            }

            ptcl_pos[slot] = float4(candidate, 0.0);
            // The velocity family is WINDOW-SIZED: this cell is inside the
            // window by the D0 dispatch bound (guard anyway).
            int wv = win_field_idx(
                x, y, z, cb.window_origin_x, cb.window_origin_y,
                cb.window_origin_z, cb.window_size_x, cb.window_size_z);
            ptcl_vel[slot] = (wv >= 0)
                ? float4(vel_x[wv], vel_y[wv], vel_z[wv], 0.0)
                : float4(0.0, 0.0, 0.0, 0.0);
            ptcl_color[slot] = float4(norm_ryb, m_k);
            ptcl_alive[slot] = 1;
        }
      }
    }

    // Clamp own cell to non-negative (neighbors handled by their own threads).
    // The host seeds density_out/color_*_out with current values, so
    // subtraction may leave negative values; clamp to 0 here via atomic max.
    // Neighbor cells are clamped by their own threads (each thread runs the
    // same shader). The *_out tmps are window-sized — index window-locally.
    {
        int w0 = win_field_idx(
            x, y, z, cb.window_origin_x, cb.window_origin_y,
            cb.window_origin_z, cb.window_size_x, cb.window_size_z);
        if (w0 >= 0) {
            atomicFloatMax(density_out, uint(w0) * 4u, 0.0);
            atomicFloatMax(color_r_out, uint(w0) * 4u, 0.0);
            atomicFloatMax(color_y_out, uint(w0) * 4u, 0.0);
            atomicFloatMax(color_b_out, uint(w0) * 4u, 0.0);
        }
    }
}
