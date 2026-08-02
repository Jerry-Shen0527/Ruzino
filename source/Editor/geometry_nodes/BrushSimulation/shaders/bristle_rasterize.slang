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

// Deposit one sample point into its (window-local) cell. Carries the full
// per-sample payload (density + color + velocity) and writes into all 7
// accumulation grids. No-op outside the active window / Z extent.
void deposit_point(float3 pos, float4 col, float3 vel,
                   float deposit, float z_weight_at_center)
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

    // Spread over ~3 cells in Z around the sample to create a paint blob.
    // XY is a single cell (no spatial kernel): bristle_rasterize only runs
    // over the CURRENT frame's samples (the accumulation grid is cleared and
    // rebuilt each sub-step), so an XY kernel here would smooth only the
    // freshly-deposited region — producing a visible disc that follows the
    // brush tip, with a hard edge where old (un-smoothed) paint begins.
    // Paper §5.2's smoothing kernel is for particle→grid transfer applied
    // to the whole field, not a per-sample splat. Smoothness here comes
    // from dense bristle sampling (80 bristles × 128 samples) + sub-stepping.
    int z_spread = min(3, D);
    if (ix < 0 || ix >= N || iy < 0 || iy >= N) return;

    // The bristle accumulation grids are WINDOW-SIZED (WIN_XY × WIN_XY × D),
    // so the write index is window-local: global cell minus window_origin.
    // Paper §5.1: bristle samples only exist in the brush-local window, so
    // the splat target is always inside [origin, origin+WIN). Cells outside
    // the window are skipped.
    int wox = cb.window_origin_x, woy = cb.window_origin_y;
    int lx = ix - wox, ly = iy - woy;
    int WSX = cb.window_size_x;
    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX) return;

    for (int dz = 0; dz < z_spread; dz++) {
        int iz = iz_center + dz - 1;
        if (iz < 0 || iz >= D) continue;

        // Paper §4.2: dried cells become solid in PRESSURE PROJECTION, not
        // here. The bristle→grid rasterize splats the sample at its actual
        // position; the fluid solve treats dry cells as solid walls. No
        // deposit-climbing hack (it was resolution-dependent and broke
        // strokes at 512).

        float z_weight = (1.0 - 0.3 * abs(float(dz - 1))) * z_weight_at_center;

        // Window-local linear index into the window-sized buffer.
        uint gi = uint(iz) * uint(WSX * WSX) + uint(ly) * uint(WSX) + uint(lx);
        bristle_density[gi] += deposit * z_weight;
        bristle_vel_x[gi] += vel.x * deposit * z_weight;
        bristle_vel_y[gi] += vel.y * deposit * z_weight;
        bristle_vel_z[gi] += vel.z * deposit * z_weight;
        bristle_color_r[gi] += col.x * deposit * z_weight;
        bristle_color_y[gi] += col.y * deposit * z_weight;
        bristle_color_b[gi] += col.z * deposit * z_weight;
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

    float base_deposit = col.w * 0.15;

    // Single-point XY splat with Z spread. Smoothness comes from dense
    // bristle sampling + host sub-stepping, not a per-sample XY kernel (an
    // XY kernel here would smooth only the current frame's deposit region,
    // producing a visible disc following the brush tip).
    deposit_point(pos, col, vel, base_deposit, 1.0);
}
