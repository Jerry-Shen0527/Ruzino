// Bristle rasterization — splat sample density to 3D accumulation grid.
//
// ANTI-ZEBRA SWEEP (bristle-driven, paper-faithful):
// Each bristle sample is splatted along the prev→current brush displacement
// arc, so contact is continuous along the drag trajectory (the brush
// physically sweeps through the medium, §4.1). Without this, fast frame-to-
// frame brush motion leaves isolated blobs separated by empty gaps (the
// "zebra stripe" symptom).
//
// IMPLEMENTATION: one GPU thread per (sample, sweep-step). The host dispatches
// Nb*S*sweep_steps threads — so the full swept arc is covered in a SINGLE
// dispatch, no per-thread loop, no step cap. Each thread computes its own
// sample index and step index from its global thread id, then deposits at
// the interpolated position along prev→current.
//
// Per-sample deposit mass is divided by sweep_steps so total paint per unit
// length is frame-rate independent (a fast stroke and a slow stroke lay down
// the same amount of paint). When sweep_steps==1 (or no prev pos), this
// degenerates to the original single-point splat.
//
// Per-sample prev position is approximated as
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
    int total_samples = cb.num_bristles * cb.samples_per_bristle;
    int total_threads = total_samples * cb.sweep_steps;
    if (int(tid.x) >= total_threads) return;

    // Decode (sample_idx, step_idx) from linear thread id.
    int sample_idx = int(tid.x) / cb.sweep_steps;
    int step_idx   = int(tid.x) % cb.sweep_steps;

    float3 pos = sample_pos[sample_idx].xyz;
    float4 col = sample_color[sample_idx];  // r, y, b, density
    float3 vel = sample_vel[sample_idx].xyz;

    float base_deposit = col.w * 0.15;

    if (cb.has_prev_brush_pos != 0 && cb.sweep_steps > 1) {
        float3 brush_delta = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z)
                           - float3(cb.prev_brush_pos_x, cb.prev_brush_pos_y, cb.prev_brush_pos_z);
        // Interpolated position along prev→current.
        // t=0 at prev, t=1 at current. Step s of [0, sweep_steps) maps to
        // t = s/(sweep_steps-1); but to avoid degenerate overlap at endpoints
        // we use t = (s+0.5)/sweep_steps (cell-centered sampling on the arc).
        float t = (float(step_idx) + 0.5) / float(cb.sweep_steps);
        float3 p = pos - brush_delta * (1.0 - t);  // prev (t=0) → current (t=1)
        // Per-step weight keeps total deposit independent of sweep_steps.
        float per_step_weight = 1.0 / float(cb.sweep_steps);
        deposit_point(p, col, vel, base_deposit, per_step_weight);
    } else {
        // No sweep (first frame or no motion): splat only at current point.
        // Only the step_idx==0 thread should fire to avoid double-deposit.
        if (step_idx == 0) {
            deposit_point(pos, col, vel, base_deposit, 1.0);
        }
    }
}
