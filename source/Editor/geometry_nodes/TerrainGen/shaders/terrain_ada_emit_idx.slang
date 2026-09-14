// Index emission: one thread per tile, serial loop over the tile's quad
// cells. Mirrors AdaMesh.hpp's emit_tile_indices exactly (cyclic boundary
// ring + clean-apex fan); terrain_ada_stitch_test pins that reference down
// (watertight over all 16 stitch combinations).

#include "terrain_ada_common.slangh"

ConstantBuffer<AdaConstants> cb : register(b0);

StructuredBuffer<float> tile_level : register(t0);
StructuredBuffer<float> vertex_base : register(t1);
StructuredBuffer<float> index_base : register(t2);

RWStructuredBuffer<uint> indices : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const int T = cb.tiles;
    const int tile = int(dtid.x);
    if (tile >= T * T) return;

    const int C = cb.tile_cells;
    const int level = int(tile_level[uint(tile)] + 0.5f);
    const int s = C >> level;

    const int tx = tile % T;
    const int ty = tile / T;
    const int stitchN = (ada_level_at(tile_level, T, tx, ty + 1) == level + 1) ? 1 : 0;
    const int stitchE = (ada_level_at(tile_level, T, tx + 1, ty) == level + 1) ? 1 : 0;
    const int stitchS = (ada_level_at(tile_level, T, tx, ty - 1) == level + 1) ? 1 : 0;
    const int stitchW = (ada_level_at(tile_level, T, tx - 1, ty) == level + 1) ? 1 : 0;

    const uint vbase = uint(vertex_base[uint(tile)]);
    uint obase = uint(index_base[uint(tile)]);

    const int d = ada_interior_dim(C, s);
    const int nseg = C / s;
    int mid_base[4];
    ada_side_mid_base(C, s, stitchN, stitchE, stitchS, stitchW, mid_base);

    for (int b = 0; b < nseg; ++b) {
        for (int a = 0; a < nseg; ++a) {
            const uint p00 = uint(ada_interior_slot(C, s, a, b));
            const uint p10 = uint(ada_interior_slot(C, s, a + 1, b));
            const uint p11 = uint(ada_interior_slot(C, s, a + 1, b + 1));
            const uint p01 = uint(ada_interior_slot(C, s, a, b + 1));

            // Cyclic boundary walk, CCW from +Y, starting at p00.
            uint ring[8];
            int n = 0;
            ring[n++] = p00;
            if (stitchW != 0 && a == 0)
                ring[n++] = uint(ada_mid_id(mid_base, 2, b));  // W
            ring[n++] = p01;
            if (stitchN != 0 && b == nseg - 1)
                ring[n++] = uint(ada_mid_id(mid_base, 1, a));  // N
            ring[n++] = p11;
            if (stitchE != 0 && a == nseg - 1)
                ring[n++] = uint(ada_mid_id(mid_base, 3, b));  // E
            ring[n++] = p10;
            if (stitchS != 0 && b == 0)
                ring[n++] = uint(ada_mid_id(mid_base, 0, a));  // S

            // Clean-apex fan (mirrors AdaMesh.hpp): a corner whose two
            // adjacent sides carry no mids.
            const bool eff_n = (stitchN != 0) && (b == nseg - 1);
            const bool eff_e = (stitchE != 0) && (a == nseg - 1);
            const bool eff_s = (stitchS != 0) && (b == 0);
            const bool eff_w = (stitchW != 0) && (a == 0);
            uint apex = p00;
            if (eff_w && eff_s) apex = p11;
            else if (eff_s && eff_e) apex = p01;
            else if (eff_n && eff_e) apex = p00;
            else if (eff_n && eff_w) apex = p10;
            else if (eff_w) apex = p11;
            else if (eff_s) apex = p01;
            else if (eff_n) apex = p10;
            else if (eff_e) apex = p00;

            int apex_pos = 0;
            for (int k = 0; k < n; ++k) {
                if (ring[k] == apex) {
                    apex_pos = k;
                    break;
                }
            }
            for (int k = 1; k + 1 < n; ++k) {
                indices[obase++] = vbase + ring[apex_pos];
                indices[obase++] = vbase + ring[(apex_pos + k) % n];
                indices[obase++] = vbase + ring[(apex_pos + k + 1) % n];
            }
        }
    }
}
