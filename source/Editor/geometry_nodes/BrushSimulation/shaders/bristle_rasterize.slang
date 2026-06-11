// Bristle rasterization — splat sample density to 3D accumulation grid
// One thread per sample. Each sample writes to its OWN 3D cell only.
// Bristles are in the XY plane; Z deposit depends on bristle height (z component).

#include "common.slangh"

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<float2> sample_pos : register(t0);     // Nb * S
StructuredBuffer<float4> sample_color : register(t1);   // Nb * S
StructuredBuffer<float2> sample_vel : register(t2);     // Nb * S
// 3D accumulation grids — res × res × res_z
RWStructuredBuffer<float> bristle_density : register(u0);
RWStructuredBuffer<float> bristle_vel_x : register(u1);
RWStructuredBuffer<float> bristle_vel_y : register(u2);
RWStructuredBuffer<float> bristle_vel_z : register(u3);
RWStructuredBuffer<float> bristle_color_r : register(u4);
RWStructuredBuffer<float> bristle_color_y : register(u5);
RWStructuredBuffer<float> bristle_color_b : register(u6);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int total = cb.num_bristles * cb.samples_per_bristle;
    if (idx >= total) return;

    float2 pos = sample_pos[idx];
    float4 col = sample_color[idx]; // r, y, b, density
    float2 vel = sample_vel[idx];

    int N = cb.grid_res;
    int D = cb.grid_res_z;
    float half = cb.paper_size * 0.5;

    // World → grid XY coords
    float gx = (pos.x - cb.grid_center_x + half) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half) / cb.cell_size;

    int ix = int(floor(gx));
    int iy = int(floor(gy));

    // Z: deposit at a few layers near the base (bristles touch the surface)
    // Spread over ~3 cells in Z to create a paint blob
    int iz_center = 0; // bristles deposit at z=0 (surface layer)
    int z_spread = min(3, D);

    if (ix < 0 || ix >= N || iy < 0 || iy >= N) return;

    float deposit = col.w * 0.15;

    for (int dz = 0; dz < z_spread; dz++) {
        int iz = iz_center + dz;
        if (iz < 0 || iz >= D) continue;

        // Vertical falloff: strongest at surface, weaker above
        float z_weight = 1.0 - 0.3 * float(dz);
        uint gi = grid_idx_3d((uint)ix, (uint)iy, (uint)iz, N, D);

        bristle_density[gi] += deposit * z_weight;
        bristle_vel_x[gi] += vel.x * deposit * z_weight;
        bristle_vel_y[gi] += vel.y * deposit * z_weight;
        bristle_vel_z[gi] += 0.0; // bristles don't inject vertical velocity
        bristle_color_r[gi] += col.x * deposit * z_weight;
        bristle_color_y[gi] += col.y * deposit * z_weight;
        bristle_color_b[gi] += col.z * deposit * z_weight;
    }
}
