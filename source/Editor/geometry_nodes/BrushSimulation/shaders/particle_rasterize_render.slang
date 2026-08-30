// Render-time deposit PREVIEW raster (window-sized), consumed ONLY by
// pack_float4's swarm composite. Re-rasterizes the SAME live particles as
// particle_rasterize.slang, but with Eq.16's deposit kernel (W_smooth_3d,
// h = 2.5 cells over a 5x5x5 neighborhood, discretely normalized so each
// particle contributes exactly its mass, SUM w_c = 1) and with no slow/far
// gate — every alive particle is real ink about to join the canvas.
//
// WHY A SECOND RASTER: the sim raster (particle_rasterize.slang) splats at
// 1-cell trilinear width because the Section 4.3 velocity merge wants a
// compact kernel. pack_float4 composites that raster into the global grid
// before rendering, so the same mass that Eq.16 later spreads at 2.5-cell
// width rendered ~6x denser inside the active window than the identical mass
// sitting on the canvas outside it. After the d/0.3 density normalization the
// window interior saturated (AO followed), and the window boundary showed up
// as a dark seam around the wet region. Rasterizing with the deposit kernel
// makes the composite a true PREVIEW of what these cells will look like once
// the swarm deposits: window interior ~= post-deposit trail, seam gone.
//
// Render-only: writes just the 4 pack-relevant raster buffers (density +
// mass-weighted RYB), which the commit node clears before this dispatch and
// the fluid node clears again at the top of the next frame. No sim state is
// touched, so the mass ledger is unaffected.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_color : register(t1);
StructuredBuffer<uint> ptcl_alive : register(t2);
RWByteAddressBuffer ptcl_density : register(u0);
RWByteAddressBuffer ptcl_color_r : register(u1);
RWByteAddressBuffer ptcl_color_y : register(u2);
RWByteAddressBuffer ptcl_color_b : register(u3);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float3 pos = ptcl_pos[idx].xyz;
    float4 col = ptcl_color[idx];  // RYB pigment + mass in w

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    int res = cb.grid_res, res_z = cb.grid_res_z;

    int cx = int(floor((pos.x - cb.grid_center_x + half_p) / cb.cell_size));
    int cy = int(floor((pos.y - cb.grid_center_y + half_p) / cb.cell_size));
    int cz = int(floor((pos.z - cb.grid_center_z + half_h) / cell_z));

    // Same kernel as particle_to_grid (Eq.16): 2.5-cell radius on a 5x5x5
    // neighborhood. The raster buffers are window-sized, so cells outside the
    // window (the apron past the window edge) are skipped — they have no
    // reader (pack composites window cells only).
    float h = cb.cell_size * 2.5;
    int WSX = cb.window_size_x;
    int wox = cb.window_origin_x, woy = cb.window_origin_y;

    float wsum = 0.0;
    [unroll] for (int dz = -2; dz <= 2; dz++) {
      [unroll] for (int dy = -2; dy <= 2; dy++) {
        [unroll] for (int dx = -2; dx <= 2; dx++) {
            int ix = cx + dx, iy = cy + dy, iz = cz + dz;
            if (ix < 0 || ix >= res || iy < 0 || iy >= res || iz < 0 || iz >= res_z)
                continue;
            int lnx = ix - wox, lny = iy - woy;
            if (lnx < 0 || lnx >= WSX || lny < 0 || lny >= WSX)
                continue;
            float3 cell_pos = float3(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(iz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            wsum += W_smooth_3d(pos - cell_pos, h);
        }
      }
    }
    if (wsum < 1e-9) return;  // no valid window cell in range

    [unroll] for (int dz = -2; dz <= 2; dz++) {
      [unroll] for (int dy = -2; dy <= 2; dy++) {
        [unroll] for (int dx = -2; dx <= 2; dx++) {
            int ix = cx + dx, iy = cy + dy, iz = cz + dz;
            if (ix < 0 || ix >= res || iy < 0 || iy >= res || iz < 0 || iz >= res_z)
                continue;
            int lnx = ix - wox, lny = iy - woy;
            if (lnx < 0 || lnx >= WSX || lny < 0 || lny >= WSX)
                continue;
            float3 cell_pos = float3(
                (float(ix) + 0.5) * cb.cell_size - half_p + cb.grid_center_x,
                (float(iy) + 0.5) * cb.cell_size - half_p + cb.grid_center_y,
                (float(iz) + 0.5) * cell_z - half_h + cb.grid_center_z);
            float w = W_smooth_3d(pos - cell_pos, h) / wsum;
            // Window-local linear index, same layout as particle_rasterize.
            uint gi = uint(iz) * uint(WSX * WSX) + uint(lny) * uint(WSX) + uint(lnx);
            float m = col.w * w;
            atomicFloatAdd(ptcl_density, gi * 4u, m);
            atomicFloatAdd(ptcl_color_r, gi * 4u, col.x * m);
            atomicFloatAdd(ptcl_color_y, gi * 4u, col.y * m);
            atomicFloatAdd(ptcl_color_b, gi * 4u, col.z * m);
        }
      }
    }
}
