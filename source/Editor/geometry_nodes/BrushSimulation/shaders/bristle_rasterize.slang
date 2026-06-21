// Bristle rasterization — splat sample density to 3D accumulation grid.
//
// ANTI-ZEBRA SWEEP: each sample is splatted along the prev→current brush
// displacement arc (the brush physically sweeps through the medium, §4.1).
// Without sweep, fast frame-to-frame brush motion leaves isolated blobs
// separated by empty gaps (the "zebra stripe" symptom) because the raster
// only fires at the single contact point of the current frame. With sweep,
// deposit is continuous along the drag trajectory.
//
// The per-sample prev position is approximated as
//   sample_pos_prev = sample_pos - (brush_pos - prev_brush_pos)
// i.e. every sample translates by the brush-center delta. Bristle DEFLECTION
// relative to the brush center is small versus the sweep length, so this is
// a faithful approximation of the swept bristle contact.

#include "common.slangh"

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<float4> sample_pos : register(t0);     // Nb * S (.xyz = 3D pos)
StructuredBuffer<float4> sample_color : register(t1);   // Nb * S
StructuredBuffer<float4> sample_vel : register(t2);     // Nb * S (.xyz = 3D vel)
// 3D accumulation grids — window-sized (window_size_x × window_size_x × window_size_z)
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

    int WSX = cb.window_size_x, WSZ = cb.window_size_z;
    for (int dz = 0; dz < z_spread; dz++) {
        int iz = iz_center + dz - 1;  // centered on the sample's Z cell
        if (iz < 0 || iz >= D) continue;

        // Vertical falloff: strongest at center, weaker above/below.
        float z_weight = (1.0 - 0.3 * abs(float(dz - 1))) * z_weight_at_center;

        int lx = ix - cb.window_origin_x;
        int ly = iy - cb.window_origin_y;
        int lz = iz - cb.window_origin_z;
        if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX || lz < 0 || lz >= WSZ)
            continue;
        uint gi = grid_idx_3d((uint)lx, (uint)ly, (uint)lz, WSX, WSZ);

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
    int idx = int(tid.x);
    int total = cb.num_bristles * cb.samples_per_bristle;
    if (idx >= total) return;

    float3 pos = sample_pos[idx].xyz;
    float4 col = sample_color[idx];  // r, y, b, density
    float3 vel = sample_vel[idx].xyz;

    float deposit = col.w * 0.15;

    // ---- Sweep along prev→current brush motion ----
    // If we have a valid previous brush position, deposit along the swept
    // arc. Step size is half a cell so adjacent deposits overlap → no gaps.
    // If the brush barely moved this frame (or first frame), fall back to a
    // single-point splat.
    if (cb.has_prev_brush_pos != 0) {
        float3 brush_delta = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z)
                           - float3(cb.prev_brush_pos_x, cb.prev_brush_pos_y, cb.prev_brush_pos_z);
        float sweep_len = length(brush_delta);
        float step = cb.cell_size * 0.5;
        // Cap the number of steps to keep warp cost bounded on very fast
        // strokes; if motion exceeds this, the next frame continues the arc.
        const int MAX_STEPS = 64;
        int n_steps = max(1, int(ceil(sweep_len / step)));
        if (n_steps > MAX_STEPS) n_steps = MAX_STEPS;
        // Distribute the per-sample deposit mass across the swept steps so
        // the total deposited paint is independent of frame rate: a fast
        // stroke (long sweep, many steps) and a slow stroke (short sweep,
        // few steps) put down the same amount of paint per unit length.
        float per_step_weight = 1.0 / float(n_steps);
        for (int s = 0; s < n_steps; s++) {
            float t = (n_steps == 1) ? 0.0 : float(s) / float(n_steps - 1);
            float3 p = pos - brush_delta * (1.0 - t);  // prev (t=0) → current (t=1)
            deposit_point(p, col, vel, deposit, per_step_weight);
        }
    } else {
        // First frame: no motion history, splat only at the current point.
        deposit_point(pos, col, vel, deposit, 1.0);
    }
}
