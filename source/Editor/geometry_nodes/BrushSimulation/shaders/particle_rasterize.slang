// Particle rasterization — splat particle density/velocity/color to 3D accum grids
// max_particles threads, alive only. Trilinear splat to a 2×2×2 cell block.
// Low collision probability since particles are spread across the volume;
// rare races are compensated by merge normalization.
// Rasterizes RYB color channels weighted by particle mass for proper color transfer.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_color : register(t1);
StructuredBuffer<float4> ptcl_vel : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
// Accumulation grids, bound as raw views: every splat is an atomicFloatAdd
// CAS (plain += raced; the raster feeds the §4.3 joint velocity merge and
// the pack-time composite, so lost/garbled accumulations corrupted both the
// FLIP/PIC update and the rendered swarm mass).
RWByteAddressBuffer ptcl_density : register(u0);
RWByteAddressBuffer ptcl_vel_x : register(u1);
RWByteAddressBuffer ptcl_vel_y : register(u2);
RWByteAddressBuffer ptcl_vel_z : register(u3);
// Color channels: RYB weighted by mass (for merge into main grid)
RWByteAddressBuffer ptcl_color_r : register(u4);
RWByteAddressBuffer ptcl_color_y : register(u5);
RWByteAddressBuffer ptcl_color_b : register(u6);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;

    float3 pos = ptcl_pos[idx].xyz;
    float4 col = ptcl_color[idx];  // RYB pigment + mass in w
    float3 vel = ptcl_vel[idx].xyz;

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size;
    float gz = (pos.z - cb.grid_center_z + half_h) / cell_z;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    int iz = int(floor(gz));

    // Trilinear splat to 2×2×2 cells
    float fx = gx - float(ix);
    float fy = gy - float(iy);
    float fz = gz - float(iz);

    float deposit = col.w;
    float3 color_deposit = col.rgb * col.w;

    int res = cb.grid_res, res_z = cb.grid_res_z;
    // The particle rasterize grids are WINDOW-SIZED (WIN_XY × WIN_XY × res_z),
    // so the splat index is window-local: global cell minus window_origin.
    // Paper §5: particles only exist in the brush-local window, so the splat
    // target is always inside [origin, origin+WIN). Cells outside the window
    // (including the 2×2×2 trilinear apron that falls past the window edge)
    // are skipped.
    int wox = cb.window_origin_x, woy = cb.window_origin_y;
    int WSX = cb.window_size_x;
    [unroll] for (int dz = 0; dz <= 1; dz++) {
        [unroll] for (int dy = 0; dy <= 1; dy++) {
            [unroll] for (int dx = 0; dx <= 1; dx++) {
                int nx = ix + dx;
                int ny = iy + dy;
                int nz = iz + dz;
                if (nx < 0 || nx >= res || ny < 0 || ny >= res || nz < 0 || nz >= res_z)
                    continue;
                int lnx = nx - wox, lny = ny - woy;
                if (lnx < 0 || lnx >= WSX || lny < 0 || lny >= WSX)
                    continue;

                float wx = (dx == 0) ? (1.0 - fx) : fx;
                float wy = (dy == 0) ? (1.0 - fy) : fy;
                float wz = (dz == 0) ? (1.0 - fz) : fz;
                float w = wx * wy * wz;
                // Window-local linear index into the window-sized buffer.
                uint gi = uint(nz) * uint(WSX * WSX) + uint(lny) * uint(WSX) + uint(lnx);

                // Atomic CAS adds: many particles splat into the same cell
                // every frame (the swarm under the brush is dense by
                // design), so the racy plain += lost mass routinely, not
                // rarely. See common.slangh atomicFloatAdd.
                atomicFloatAdd(ptcl_density, gi * 4u, deposit * w);
                atomicFloatAdd(ptcl_vel_x, gi * 4u, vel.x * deposit * w);
                atomicFloatAdd(ptcl_vel_y, gi * 4u, vel.y * deposit * w);
                atomicFloatAdd(ptcl_vel_z, gi * 4u, vel.z * deposit * w);
                atomicFloatAdd(ptcl_color_r, gi * 4u, color_deposit.r * w);
                atomicFloatAdd(ptcl_color_y, gi * 4u, color_deposit.g * w);
                atomicFloatAdd(ptcl_color_b, gi * 4u, color_deposit.b * w);
            }
        }
    }
}
