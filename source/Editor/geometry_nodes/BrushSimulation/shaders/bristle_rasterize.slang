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
// Main-grid wetness + density (read-only) to detect dried (solid) cells.
// Bristle paint must NOT be deposited into a solid cell — it piles up on top
// instead, so a new stroke covers a dried stroke rather than mixing into it
// in-place.
StructuredBuffer<float> grid_wetness : register(t3);
StructuredBuffer<float> grid_density : register(t4);
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
    int z_spread = min(3, D);
    if (ix < 0 || ix >= N || iy < 0 || iy >= N) return;

    for (int dz = 0; dz < z_spread; dz++) {
        int iz = iz_center + dz - 1;  // centered on the sample's Z cell
        if (iz < 0 || iz >= D) continue;

        // Coverage: a cell that carries dried paint (density > 0 AND
        // wetness < threshold) is solid — new paint cannot enter it. Walk
        // upward from iz to find the first non-solid cell and deposit there
        // instead. This makes a new stroke pile on TOP of a dried stroke
        // (coverage) rather than mixing into it in-place. Empty cells
        // (density == 0) are NOT solid, even when wetness is 0 — paint can
        // always be laid on bare canvas. Wet paint cells accept new paint
        // normally, allowing wet-in-wet blending.
        int iz_target = iz;
        for (int step = 0; step < D; ++step) {
            if (iz_target >= D) break;
            uint gi_test = grid_idx_3d((uint)ix, (uint)iy, (uint)iz_target, N, D);
            bool solid = grid_density[gi_test] > 1e-6f &&
                         grid_wetness[gi_test] < 0.01f;
            if (!solid) break;  // empty or wet: deposit here
            iz_target++;  // solid dried paint: keep climbing
        }
        if (iz_target >= D) continue;  // solid all the way up: nowhere to put it

        // Vertical falloff: strongest at center, weaker above/below.
        float z_weight = (1.0 - 0.3 * abs(float(dz - 1))) * z_weight_at_center;

        // Global grid index (buffers are global-sized).
        uint gi = grid_idx_3d((uint)ix, (uint)iy, (uint)iz_target, N, D);

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

    // Single-point splat. Continuity along fast strokes is achieved on the
    // host via sub-stepping (multiple complete deposits along the trajectory),
    // not by sweeping a single splat inside this shader.
    deposit_point(pos, col, vel, base_deposit, 1.0);
}
