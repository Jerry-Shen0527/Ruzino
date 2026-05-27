// Brush deposit shader — deposits paint from curve vertices into 2D grid
// Single-threaded to avoid race conditions; processes all vertices sequentially.
// Uses brightness-preserving RYB color mixing (Wetbrush Algorithm 2).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

// Vertex data (uploaded from CPU, new vertices only)
StructuredBuffer<float4> vertices : register(t0);  // (x, y, z, timestamp)
StructuredBuffer<float4> colors   : register(t1);  // (r, y, b, pressure)

// Grid fields (RW)
RWStructuredBuffer<float> density  : register(u0);
RWStructuredBuffer<float> color_r  : register(u1);
RWStructuredBuffer<float> color_y  : register(u2);
RWStructuredBuffer<float> color_b  : register(u3);
RWStructuredBuffer<float> vel_x    : register(u4);
RWStructuredBuffer<float> vel_y    : register(u5);
RWStructuredBuffer<float> wetness  : register(u6);
RWStructuredBuffer<float> height   : register(u7);

[numthreads(1, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    float half_paper = cb.paper_size * 0.5f;
    int spread = max(1, (int)ceil(cb.effective_radius / cb.cell_size));

    float prev_gx = 0.0f, prev_gy = 0.0f;
    float prev_time = -1.0f;

    for (int vi = 0; vi < cb.num_vertices; vi++)
    {
        float4 pos = vertices[vi];
        float4 col = colors[vi];

        // World → grid coordinates
        float gx = (pos.x - cb.center_x + half_paper) / cb.cell_size;
        float gy = (pos.y - cb.center_y + half_paper) / cb.cell_size;

        int cx = (int)gx;
        int cy = (int)gy;

        float inv_r = (cb.effective_radius > 1e-8f) ?
                       1.0f / cb.effective_radius : 0.0f;

        // Deposit paint in neighborhood
        for (int dy = -spread; dy <= spread; dy++)
        {
            for (int dx = -spread; dx <= spread; dx++)
            {
                int px = cx + dx;
                int py = cy + dy;
                if (px < 0 || px >= N || py < 0 || py >= N) continue;

                float dist = sqrt((float)(dx*dx + dy*dy)) * cb.cell_size;
                if (dist >= cb.effective_radius) continue;

                float w = 1.0f - dist * inv_r;
                w *= w;
                float dep = cb.ink_amount * 0.1f * w;

                uint idx = grid_idx((uint)px, (uint)py, N);
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
                height[idx] += dep;
            }
        }

        // Inject velocity from brush motion
        if (vi > 0 && prev_time >= 0.0f)
        {
            float dt_v = pos.w - prev_time;
            if (dt_v > 1e-6f)
            {
                // Grid-space velocity in Stam units (grid / N per second)
                float vx_stam = (gx - prev_gx) / dt_v / (float)N;
                float vy_stam = (gy - prev_gy) / dt_v / (float)N;

                for (int dy2 = -spread; dy2 <= spread; dy2++)
                {
                    for (int dx2 = -spread; dx2 <= spread; dx2++)
                    {
                        int px2 = cx + dx2;
                        int py2 = cy + dy2;
                        if (px2 < 0 || px2 >= N || py2 < 0 || py2 >= N) continue;

                        float dist2 = sqrt((float)(dx2*dx2 + dy2*dy2)) * cb.cell_size;
                        if (dist2 >= cb.effective_radius) continue;

                        float w2 = 1.0f - dist2 / cb.effective_radius;
                        uint idx2 = grid_idx((uint)px2, (uint)py2, N);
                        vel_x[idx2] += vx_stam * w2;
                        vel_y[idx2] += vy_stam * w2;
                    }
                }
            }
        }

        prev_gx = gx;
        prev_gy = gy;
        prev_time = pos.w;
    }
}
