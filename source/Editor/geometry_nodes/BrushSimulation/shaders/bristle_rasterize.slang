// Bristle rasterization — splat sample density to 3D accumulation grid.
//
// Wetbrush §4.1: "Once we update all the sample positions and their local
// frames, we rasterize them into a density field and a velocity field."
// One thread per bristle sample; each sample splats its full payload
// (density + color + velocity) at its current position into the window-local
// accumulation grid.
//
// CONTINUITY (anti-zebra) is handled on the HOST, not here. The host runs
// the full bristle pipeline (sim → rasterize → merge) as a sequence of
// sub-steps along the prev→current brush trajectory when the frame-to-frame
// brush displacement exceeds one brush diameter — this is the paper's
// "densely sampled stroke path" (§Limitations: the paper itself states it
// relies on small time steps / dense stroke sampling for continuity). Each
// sub-step is a complete deposit at an interpolated brush position, so the
// brush footprint overlaps along the trajectory with no gaps. This shader
// only ever does a single-point splat; the sweep_steps/prev_brush_pos fields
// in BristleConstants are retained for ABI compatibility but unused.

#include "common.slangh"

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<float4> sample_pos : register(t0);     // Nb * S (.xyz = 3D pos)
StructuredBuffer<float4> sample_color : register(t1);   // Nb * S
StructuredBuffer<float4> sample_vel : register(t2);     // Nb * S (.xyz = 3D vel)
// 3D accumulation grids — global-sized (grid_res × grid_res × grid_res_z)
RWStructuredBuffer<float> bristle_density : register(u0);
RWStructuredBuffer<float> bristle_vel_x : register(u1);
RWStructuredBuffer<float> bristle_vel_y : register(u2);
RWStructuredBuffer<float> bristle_vel_z : register(u3);
RWStructuredBuffer<float> bristle_color_r : register(u4);
RWStructuredBuffer<float> bristle_color_y : register(u5);
RWStructuredBuffer<float> bristle_color_b : register(u6);

// Deposit one sample point into its (window-local) cells. Carries the full
// per-sample payload (density + color + velocity) and writes into all 7
// accumulation grids. No-op outside the active window / Z extent.
//
// SPLAT KERNEL: the sample's mass is spread over a 3×3×3 neighborhood with
// the paper's own Gaussian kernel W_smooth_3d (the same W used by the §5.2
// grid↔particle transfer, Eq.15/16). A single-cell XY splat left the density
// field as 600 isolated cell-columns per frame (one per bristle), which
// surfaced as speckle/凹凸 on the rendered stroke. Spreading each sample
// with the paper's kernel makes the field continuous at sub-footprint scale.
// Weights are normalized over the 27 cells so the deposit is conserved (the
// accumulated density scale — and with it the render threshold kDensitySurface
// — stays exactly where the single-cell splat left it).
void deposit_point(float3 pos, float4 col, float3 vel,
                   float deposit, float vel_deposit)
{
    int N = cb.grid_res;
    int D = cb.grid_res_z;
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(D);

    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size;
    float gz = (pos.z - cb.grid_center_z + half_h) / cell_z;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    int iz_center = int(floor(gz));

    // The bristle accumulation grids are WINDOW-SIZED (WIN_XY × WIN_XY × D),
    // so the write index is window-local: global cell minus window_origin.
    // Paper §5.1: bristle samples only exist in the brush-local window, so
    // the splat target is always inside [origin, origin+WIN). Cells outside
    // the window are skipped.
    int wox = cb.window_origin_x, woy = cb.window_origin_y;
    int WSX = cb.window_size_x;

    // 3×3×3 Gaussian kernel (paper W_smooth_3d, h = 1.5 cells — same radius
    // as particle_to_grid's Eq.16). First pass: collect raw weights so we can
    // normalize (the 27-cell truncation would otherwise leak a few percent).
    const float h = cb.cell_size * 1.5;
    float wsum = 0.0;
    float wcell[27];
    int wc = 0;
    [unroll] for (int dz = -1; dz <= 1; dz++) {
      [unroll] for (int dy = -1; dy <= 1; dy++) {
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            int nx = ix + dx, ny = iy + dy, nz = iz_center + dz;
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || nz < 0 || nz >= D) { wcell[wc++] = 0.0; continue; }
            int lnx = nx - wox, lny = ny - woy;
            if (lnx < 0 || lnx >= WSX || lny < 0 || lny >= WSX) { wcell[wc++] = 0.0; continue; }
            float3 cpos = float3(
                (float(nx) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(ny) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(nz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            float w = W_smooth_3d(pos - cpos, h);
            wcell[wc++] = w;
            wsum += w;
        }
      }
    }
    if (wsum < 1e-12) return;
    float inv_sum = 1.0 / wsum;

    wc = 0;
    [unroll] for (int dz = -1; dz <= 1; dz++) {
      [unroll] for (int dy = -1; dy <= 1; dy++) {
        [unroll] for (int dx = -1; dx <= 1; dx++) {
            float w = wcell[wc++];
            if (w <= 0.0) continue;
            int nx = ix + dx, ny = iy + dy, nz = iz_center + dz;
            int lnx = nx - wox, lny = ny - woy;

            float z_weight = w * inv_sum;

            // Window-local linear index into the window-sized buffer.
            uint gi = uint(nz) * uint(WSX * WSX) + uint(lny) * uint(WSX) + uint(lnx);
            bristle_density[gi] += deposit * z_weight;
            bristle_vel_x[gi] += vel.x * vel_deposit * z_weight;
            bristle_vel_y[gi] += vel.y * vel_deposit * z_weight;
            bristle_vel_z[gi] += vel.z * vel_deposit * z_weight;
            bristle_color_r[gi] += col.x * deposit * z_weight;
            bristle_color_y[gi] += col.y * deposit * z_weight;
            bristle_color_b[gi] += col.z * deposit * z_weight;
        }
      }
    }
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int total_samples = cb.num_bristles * cb.samples_per_bristle;
    if (int(tid.x) >= total_samples) return;

    int sample_idx = int(tid.x);

    float3 pos = sample_pos[sample_idx].xyz;
    float4 col = sample_color[sample_idx];  // r, y, b, density
    float3 vel = sample_vel[sample_idx].xyz;

    // Deposit strength. Smoothness comes from the W_smooth_3d 3×3×3 kernel
    // above (spreads each sample over 27 cells → continuous field) plus the
    // host sub-step loop, NOT from a high per-cell magnitude.
    //
    // The previous ×10 here was the trigger for the "white paint flowing
    // out / stroke widening past the brush" regression. It pushed grid
    // density to 1-5/cell — far above the 0.05 threshold of
    // particle_emit.slang mode 1 (grid→particle emission), so mode 1 fired
    // in every in-window cell every frame and minted large numbers of
    // ZERO-COLOR particles (particle_emit.slang writes color = (0,0,0,
    // density)). Those particles survive across frames and are re-rasterized
    // + re-merged into the grid every frame (particle_rasterize.slang +
    // bristle_merge.slang are additive and never consume particle mass), so
    // the zero-color mass grew unbounded and rendered as white/gray once it
    // crossed the (lowered to 0.002) render surface threshold.
    //
    // ×1.0 with the W kernel leaves the per-cell density at ~0.1-0.15/cell
    // — ~50-75× the 0.002 render threshold (still a solid slab, no speckle),
    // but only ~2-3× the mode-1 emit threshold instead of ~100×, so mode 1
    // mints far fewer zero-color particles. The render threshold was already
    // lowered 10× (0.02 → 0.002), so this 10× reduction cancels out on the
    // visible slab thickness while removing the emit-driven mass explosion.
    //
    // VELOCITY deposit keeps the OLD scale (0.15): the pressure solver's
    // fixed-point iterations converge on that magnitude; scaling velocity
    // with density exploded the divergence residual, which made the advect
    // step drag density to zero (strokes vanished).
    float density_deposit = col.w * 1.0;
    float vel_deposit = col.w * 0.15;

    // Splat with the paper's W_smooth_3d 3×3×3 kernel (normalized inside
    // deposit_point); continuity across frames comes from the host sub-step
    // loop keeping consecutive deposits within ~2 cells of each other.
    deposit_point(pos, col, vel, density_deposit, vel_deposit);
}
