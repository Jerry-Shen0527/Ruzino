// Brush deposit shader — deposits paint from curve vertices into 3D grid
// Single-threaded to avoid race conditions; processes all vertices sequentially.
// Uses brightness-preserving RYB color mixing (Wetbrush Algorithm 2).
// 3D: deposits paint in a volume column around each vertex,
// with Gaussian falloff along the Z (height) axis.
// Wetbrush: brush deposits paint in the 3D density field with sub-pixel detail.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

// Vertex data (uploaded from CPU, new vertices only)
StructuredBuffer<float4> vertices : register(t0);  // (x, y, z, timestamp)
StructuredBuffer<float4> colors   : register(t1);  // (r, y, b, pressure)

// Grid fields (RW) — 3D volume: res × res × res_z
RWStructuredBuffer<float> density  : register(u0);
RWStructuredBuffer<float> color_r  : register(u1);
RWStructuredBuffer<float> color_y  : register(u2);
RWStructuredBuffer<float> color_b  : register(u3);
RWStructuredBuffer<float> vel_x    : register(u4);
RWStructuredBuffer<float> vel_y    : register(u5);
RWStructuredBuffer<float> vel_z    : register(u6);
RWStructuredBuffer<float> wetness  : register(u7);
RWStructuredBuffer<float> oil_density : register(u8);  // §6 oil density (per-cell)
RWStructuredBuffer<float> height   : register(u9);

[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    int D = cb.res_z;
    float half_paper = cb.paper_size * 0.5f;
    float cell_z = cb.height_extent / float(D);  // Z cell size (≠ XY cell_size)
    int spread_xy = max(1, (int)ceil(cb.effective_radius / cb.cell_size));
    int spread_z = max(1, (int)ceil(cb.effective_radius / cell_z));

    float prev_gx = 0.0f, prev_gy = 0.0f;
    float prev_time = -1.0f;

    for (int vi = 0; vi < cb.num_vertices; vi++)
    {
        float4 pos = vertices[vi];
        float4 col = colors[vi];

        // World → grid coordinates
        float gx = (pos.x - cb.center_x + half_paper) / cb.cell_size;
        float gy = (pos.y - cb.center_y + half_paper) / cb.cell_size;
        // Z: the brush tip touches the canvas surface. Paint deposits UPWARD
        // from the canvas (the grid floor at z=0), never below it. The stroke
        // vertex z is the brush tip position; we treat any tip at/below the
        // canvas as contact at the surface (gz=0), so a planar stroke drawn at
        // z≈0 paints only the cells z>=0 above the paper — no mirror image
        // below. (Wetbrush §4.2: "we discretize the space ABOVE canvas".)
        float gz = (pos.z - cb.grid_center_z + cb.height_extent * 0.5f) / cell_z;
        int cx = (int)gx;
        int cy = (int)gy;
        // Clamp the deposit center to the bottom layer (canvas surface).
        // gz<0 means the tip is below the paper; paint still goes on the paper.
        int cz_center = max(0, min((int)gz, D - 1));

        float inv_r = (cb.effective_radius > 1e-8f) ?
                       1.0f / cb.effective_radius : 0.0f;

        // Deposit paint in 3D neighborhood — upper half only (dz >= 0), since
        // the canvas is a solid floor and there is no paint volume below it.
        int WSX = cb.window_size_x, WSZ = cb.window_size_z;
        for (int dz = 0; dz <= spread_z; dz++)
        {
            for (int dy = -spread_xy; dy <= spread_xy; dy++)
            {
                for (int dx = -spread_xy; dx <= spread_xy; dx++)
                {
                    int px = cx + dx;
                    int py = cy + dy;
                    int pz = cz_center + dz;
                    if (px < 0 || px >= N || py < 0 || py >= N || pz < 0 || pz >= D)
                        continue;

                    // Map GLOBAL cell to WINDOW-LOCAL index. The brush is
                    // centered in the window so deposits normally land inside;
                    // out-of-window cells are skipped (committed canvas owns them).
                    int lx = px - cb.window_origin_x;
                    int ly = py - cb.window_origin_y;
                    int lz = pz - cb.window_origin_z;
                    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX ||
                        lz < 0 || lz >= WSZ) continue;

                    float dist_xy = sqrt((float)(dx*dx + dy*dy)) * cb.cell_size;
                    float dist_z = (float)dz * cell_z;
                    float dist = sqrt(dist_xy * dist_xy + dist_z * dist_z);
                    if (dist >= cb.effective_radius) continue;

                    float w = 1.0f - dist * inv_r;
                    w *= w;
                    float dep = cb.ink_amount * 0.1f * w;

                    uint idx = grid_idx_3d((uint)lx, (uint)ly, (uint)lz, WSX, WSZ);
                    float old_d = density[idx];
                    float new_d = old_d + dep;

                    // Brightness-preserving RYB color mixing (Algorithm 2)
                    if (new_d > 1e-6f)
                    {
                        float3 old_ryb = float3(color_r[idx], color_y[idx], color_b[idx]);
                        float3 new_ryb = color_mix(col.xyz, old_ryb, dep, old_d);
                        color_r[idx] = new_ryb.r;
                        color_y[idx] = new_ryb.g;
                        color_b[idx] = new_ryb.b;
                    }

                    density[idx] = new_d;
                    wetness[idx] = min(wetness[idx] + dep * 2.0f, 1.0f);
                    // Oil density relaxes toward the user base as paint is
                    // deposited (§6: paint carries the oil content).
                    oil_density[idx] += (cb.oil_density_base - oil_density[idx]) * dep;
                    height[idx] += dep;
                }
            }
        }

        // Inject velocity from brush motion
        if (vi > 0 && prev_time >= 0.0f)
        {
            float dt_v = pos.w - prev_time;
            if (dt_v > 1e-6f)
            {
                float vx_stam = (gx - prev_gx) / dt_v / (float)N;
                float vy_stam = (gy - prev_gy) / dt_v / (float)N;

                for (int dz2 = 0; dz2 <= spread_z; dz2++)
                {
                    for (int dy2 = -spread_xy; dy2 <= spread_xy; dy2++)
                    {
                        for (int dx2 = -spread_xy; dx2 <= spread_xy; dx2++)
                        {
                            int px2 = cx + dx2;
                            int py2 = cy + dy2;
                            int pz2 = cz_center + dz2;
                            if (px2 < 0 || px2 >= N || py2 < 0 || py2 >= N
                                || pz2 < 0 || pz2 >= D) continue;

                            float dist2 = sqrt((float)(dx2*dx2 + dy2*dy2 + dz2*dz2))
                                * cb.cell_size;
                            if (dist2 >= cb.effective_radius) continue;

                            float w2 = 1.0f - dist2 / cb.effective_radius;
                            // Window-local index (same mapping as deposit above).
                            int lx2 = px2 - cb.window_origin_x;
                            int ly2 = py2 - cb.window_origin_y;
                            int lz2 = pz2 - cb.window_origin_z;
                            if (lx2 < 0 || lx2 >= WSX || ly2 < 0 || ly2 >= WSX ||
                                lz2 < 0 || lz2 >= WSZ) continue;
                            uint idx2 = grid_idx_3d((uint)lx2, (uint)ly2, (uint)lz2, WSX, WSZ);
                            vel_x[idx2] += vx_stam * w2;
                            vel_y[idx2] += vy_stam * w2;
                        }
                    }
                }
            }
        }

        prev_gx = gx;
        prev_gy = gy;
        prev_time = pos.w;
    }
}
