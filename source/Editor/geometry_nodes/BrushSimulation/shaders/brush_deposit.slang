// Brush deposit shader — deposits paint from curve vertices into 3D grid
// Single-threaded to avoid race conditions; processes all vertices sequentially.
// Uses brightness-preserving RYB color mixing (Wetbrush Algorithm 2).
// 3D: deposits paint in a volume column around each vertex,
// with Gaussian falloff along the Z (height) axis.
// Wetbrush: brush deposits paint in the 3D density field with sub-pixel detail.
//
// PATH FILL: to avoid a "zebra" gap when consecutive stroke samples are far
// apart (faster than the brush can cover at the sample rate), we deposit at
// each vertex AND interpolate along the segment to vi+1, stepping no more
// than half a cell per sub-sample so the footprint is continuous.

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

// Deposit a single paint blob at world position `wpos` with color `col`.
// `vel_inject` (if nonzero length) is the brush XY velocity (Stam-normalized)
// to write into the velocity field; pass (0,0) to skip velocity injection.
void deposit_at(float3 wpos, float4 col, float2 vel_inject)
{
    int N = cb.res;
    int D = cb.res_z;
    float half_paper = cb.paper_size * 0.5f;
    float cell_z = cb.height_extent / float(D);
    int spread_xy = max(1, (int)ceil(cb.effective_radius / cb.cell_size));
    int spread_z = max(1, (int)ceil(cb.effective_radius / cell_z));
    int WSX = cb.window_size_x, WSZ = cb.window_size_z;

    float gx = (wpos.x - cb.center_x + half_paper) / cb.cell_size;
    float gy = (wpos.y - cb.center_y + half_paper) / cb.cell_size;
    // Z: brush tip touches the canvas surface; paint deposits UPWARD only
    // (Wetbrush §4.2: "we discretize the space ABOVE canvas"). A planar
    // stroke at z≈0 paints only cells z>=0 — no mirror image below.
    float gz = (wpos.z - cb.grid_center_z + cb.height_extent * 0.5f) / cell_z;
    int cx = (int)gx;
    int cy = (int)gy;
    int cz_center = max(0, min((int)gz, D - 1));

    float inv_r = (cb.effective_radius > 1e-8f) ?
                   1.0f / cb.effective_radius : 0.0f;
    bool inject_vel = (vel_inject.x != 0.0f || vel_inject.y != 0.0f);

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

                // Map GLOBAL cell to WINDOW-LOCAL index. The brush is centered
                // in the window so deposits normally land inside; out-of-window
                // cells are skipped (committed canvas owns them).
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
                uint idx = grid_idx_3d((uint)lx, (uint)ly, (uint)lz, WSX, WSZ);

                // Always deposit paint (density + color + wetness + oil + height).
                float dep = cb.ink_amount * 0.1f * w;
                float old_d = density[idx];
                float new_d = old_d + dep;
                if (new_d > 1e-6f) {
                    float3 old_ryb = float3(color_r[idx], color_y[idx], color_b[idx]);
                    float3 new_ryb = color_mix(col.xyz, old_ryb, dep, old_d);
                    color_r[idx] = new_ryb.r;
                    color_y[idx] = new_ryb.g;
                    color_b[idx] = new_ryb.b;
                }
                density[idx] = new_d;
                wetness[idx] = min(wetness[idx] + dep * 2.0f, 1.0f);
                oil_density[idx] += (cb.oil_density_base - oil_density[idx]) * dep;
                height[idx] += dep;

                // Additionally inject brush velocity when provided.
                if (inject_vel) {
                    vel_x[idx] += vel_inject.x * w;
                    vel_y[idx] += vel_inject.y * w;
                }
            }
        }
    }
}

[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    float half_paper = cb.paper_size * 0.5f;

    float prev_gx = 0.0f, prev_gy = 0.0f;
    float prev_time = -1.0f;
    float3 prev_pos = float3(0,0,0);

    for (int vi = 0; vi < cb.num_vertices; vi++)
    {
        float4 pos = vertices[vi];
        float4 col = colors[vi];

        // Brush XY velocity (Stam-normalized) for this vertex, from the
        // displacement since the previous vertex and the time delta.
        float gx = (pos.x - cb.center_x + half_paper) / cb.cell_size;
        float gy = (pos.y - cb.center_y + half_paper) / cb.cell_size;
        float2 vel_inject = float2(0.0, 0.0);
        if (vi > 0 && prev_time >= 0.0f) {
            float dt_v = pos.w - prev_time;
            if (dt_v > 1e-6f) {
                vel_inject = float2((gx - prev_gx) / dt_v / (float)N,
                                    (gy - prev_gy) / dt_v / (float)N);
            }
        }

        // Deposit at this vertex (paint + velocity together).
        deposit_at(pos.xyz, col, vel_inject);

        // PATH FILL: interpolate between prev_pos and this vertex so the
        // footprint is continuous. Step at most half a cell per sub-sample;
        // this closes the "zebra" gaps that appear when the brush moves faster
        // than one footprint diameter between consecutive stroke samples.
        if (vi > 0) {
            float3 dseg = pos.xyz - prev_pos;
            float seg_len = length(dseg.xy);  // planar stroke length
            float step = cb.cell_size * 0.5f;  // sub-sample spacing
            int nfill = (seg_len > step) ? int(ceil(seg_len / step)) : 0;
            for (int k = 1; k <= nfill; k++) {
                float t = float(k) / float(nfill + 1);
                float3 fp = lerp(prev_pos, pos.xyz, t);
                // Interpolate velocity in time too.
                float2 fvel = vel_inject;  // constant along the segment is fine
                // Lighter deposit on interpolated points (they share ink budget
                // with the endpoints) — use full deposit so the stroke is solid.
                deposit_at(fp, col, fvel);
            }
        }

        prev_gx = gx;
        prev_gy = gy;
        prev_time = pos.w;
        prev_pos = pos.xyz;
    }
}
