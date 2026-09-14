// Per-tile vertex/index counts from the balanced level grid. Counts are
// stored as floats (exact below 2^24) so the host can read them back with
// the shared float readback helper and prefix-sum them on the CPU.

#include "terrain_ada_common.slangh"

ConstantBuffer<AdaConstants> cb : register(b0);

StructuredBuffer<float> tile_level : register(t0);

RWStructuredBuffer<float> vertex_count : register(u0);
RWStructuredBuffer<float> index_count : register(u1);

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

    vertex_count[uint(tile)] = float(
        ada_tile_vertex_count(C, s, stitchN, stitchE, stitchS, stitchW));
    index_count[uint(tile)] = float(
        ada_tile_index_count(C, s, stitchN, stitchE, stitchS, stitchW));
}
