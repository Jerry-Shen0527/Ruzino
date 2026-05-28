// Bristle rasterization — splat sample density to accumulation grid
// One thread per sample. Each sample contributes to its grid cell
// and immediate neighbors via a small kernel.

#include "common.slangh"

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<float2> sample_pos : register(t0);     // Nb * S
StructuredBuffer<float4> sample_color : register(t1);   // Nb * S
StructuredBuffer<float2> sample_vel : register(t2);     // Nb * S
RWStructuredBuffer<float> bristle_density : register(u0); // N*N
RWStructuredBuffer<float> bristle_vel_x : register(u1);   // N*N
RWStructuredBuffer<float> bristle_vel_y : register(u2);   // N*N
RWStructuredBuffer<float> bristle_color_r : register(u3); // N*N
RWStructuredBuffer<float> bristle_color_y : register(u4); // N*N
RWStructuredBuffer<float> bristle_color_b : register(u5); // N*N

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int total = cb.num_bristles * cb.samples_per_bristle;
    if (idx >= total) return;

    float2 pos = sample_pos[idx];
    float4 col = sample_color[idx]; // r, y, b, density
    float2 vel = sample_vel[idx];

    // World → grid coords
    float half = cb.paper_size * 0.5;
    float gx = (pos.x - cb.grid_center_x + half) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half) / cb.cell_size;

    int ix = int(floor(gx));
    int iy = int(floor(gy));

    // Splat to 3x3 neighborhood
    float deposit = col.w * 0.15; // per-sample deposit weight
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int nx = ix + dx;
            int ny = iy + dy;
            if (nx < 0 || nx >= cb.grid_res || ny < 0 || ny >= cb.grid_res)
                continue;

            float dist = length(float2(float(dx), float(dy)));
            float w = max(0.0, 1.0 - dist * 0.5) * deposit;
            uint gi = grid_idx(uint(nx), uint(ny), cb.grid_res);

            // Note: no atomics — concurrent writes to same cell will race.
            // This is acceptable as a rough approximation; merge step handles it.
            bristle_density[gi] += w;
            bristle_vel_x[gi] += vel.x * w;
            bristle_vel_y[gi] += vel.y * w;
            bristle_color_r[gi] += col.x * w;
            bristle_color_y[gi] += col.y * w;
            bristle_color_b[gi] += col.z * w;
        }
    }
}
