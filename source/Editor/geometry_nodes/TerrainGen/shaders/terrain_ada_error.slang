// Per-tile error -> subdivision level. One thread per tile: reduces the
// tile's height relief, max cell-edge slope, and max second difference
// (curvature), then
//   level = clamp(max(floor(log2(err / detail_threshold)),
//                     curvature level), 0, max_level)
// where the curvature level is the smallest level whose predicted flat-quad
// sag d2 * stride^2 / 8 stays under sag_tolerance. Sag QUARTERS per level
// (relief/slope halve), hence the 0.5 * log2.

#include "terrain_ada_common.slangh"

ConstantBuffer<AdaConstants> cb : register(b0);

StructuredBuffer<float> height : register(t0);

RWStructuredBuffer<float> tile_level : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const int T = cb.tiles;
    const int tile = int(dtid.x);
    if (tile >= T * T) return;

    const int C = cb.tile_cells;
    const int res = cb.res;
    const int ox = (tile % T) * C;
    const int oy = (tile / T) * C;

    float hmin = 1e30f;
    float hmax = -1e30f;
    float max_slope = 0.0f;
    float d2_max = 0.0f;
    for (int y = 0; y < C; ++y) {
        for (int x = 0; x < C; ++x) {
            const int gx = ox + x;
            const int gy = oy + y;
            const float h = height[uint(gy) * uint(res) + uint(gx)];
            hmin = min(hmin, h);
            hmax = max(hmax, h);
            // Cell-edge differences (rise over one cell of run).
            const float hx = height[uint(gy) * uint(res) + uint(gx + 1)];
            const float hy =
                height[uint(gy + 1) * uint(res) + uint(gx)];
            const float run = cb.cell_size;
            max_slope = max(max_slope, abs(hx - h) / run);
            max_slope = max(max_slope, abs(hy - h) / run);

            // Second differences: how much the surface BENDS inside the
            // tile. Flat-quad sag = d2 * stride^2 / 8; slope alone is no
            // error at all (a planar tilt is exact at any stride), only
            // changing slope is. The mixed difference carries 2x weight:
            // the diagonal twist of a triangle split bends twice as fast.
            const int xm = max(gx - 1, 0);
            const int ym = max(gy - 1, 0);
            const float h_west = height[uint(gy) * uint(res) + uint(xm)];
            const float h_south = height[uint(ym) * uint(res) + uint(gx)];
            const float h_ne =
                height[uint(gy + 1) * uint(res) + uint(gx + 1)];
            const float dxx = hx - 2.0f * h + h_west;
            const float dyy = hy - 2.0f * h + h_south;
            const float dxy = h_ne - hy - hx + h;
            d2_max = max(d2_max, max(max(abs(dxx), abs(dyy)),
                                     2.0f * abs(dxy)));
        }
    }

    const float relief_term = (hmax - hmin) * cb.inv_relief;
    const float err = max(relief_term, max_slope * cb.slope_weight);

    float level_f = 0.0f;
    if (err > cb.detail_threshold) {
        level_f = floor(log2(err / cb.detail_threshold));
    }

    float level_curv = 0.0f;
    if (cb.sag_tolerance > 0.0f) {
        const float sag0 = d2_max * float(C * C) * 0.125f;
        if (sag0 > cb.sag_tolerance) {
            level_curv = ceil(0.5f * log2(sag0 / cb.sag_tolerance));
        }
    }

    int level = int(max(level_f, level_curv));
    level = clamp(level, 0, cb.max_level);
    tile_level[uint(tile)] = float(level);
}
