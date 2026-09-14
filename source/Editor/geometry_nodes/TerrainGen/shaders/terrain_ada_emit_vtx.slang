// Vertex emission: thread = (tile, slot). Active slots write an 11-float
// vertex [pos3, normal3, uv2, water, sediment, wear] at the tile's base.
// Positions are exact lattice samples of the field, so vertices shared
// between tiles (same global cell) are bit-identical across tiles -- the
// stitch seams are watertight without any dedup.

#include "terrain_ada_common.slangh"

ConstantBuffer<AdaConstants> cb : register(b0);

StructuredBuffer<float> tile_level : register(t0);
StructuredBuffer<float> vertex_base : register(t1);
StructuredBuffer<float> height : register(t2);
StructuredBuffer<float> water : register(t3);
StructuredBuffer<float> sediment : register(t4);
StructuredBuffer<float> wear : register(t5);

RWStructuredBuffer<float> vertices : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const int T = cb.tiles;
    const int tile = int(dtid.x) / cb.max_slots;
    const int slot = int(dtid.x) % cb.max_slots;
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

    int lx, ly;
    if (!ada_slot_coords(
            C, s, stitchN, stitchE, stitchS, stitchW, slot, lx, ly))
        return;

    const int res = cb.res;
    const int gx = tx * C + lx;
    const int gy = ty * C + ly;
    const uint gi = uint(gy) * uint(res) + uint(gx);

    const float h = height[gi];

    // Analytic normal, same central-difference formula as the dense mesh
    // (clamped one-step neighbors); deterministic, so shared vertices get
    // identical normals from any tile.
    const int gx0 = max(gx - 1, 0);
    const int gx1 = min(gx + 1, res - 1);
    const int gy0 = max(gy - 1, 0);
    const int gy1 = min(gy + 1, res - 1);
    const float dhdx = (height[uint(gy) * uint(res) + uint(gx1)] -
                        height[uint(gy) * uint(res) + uint(gx0)]) /
                       (float(gx1 - gx0) * cb.cell_size);
    const float dhdy = (height[uint(gy1) * uint(res) + uint(gx)] -
                        height[uint(gy0) * uint(res) + uint(gx)]) /
                       (float(gy1 - gy0) * cb.cell_size);
    const float3 n = normalize(float3(-dhdx, 2.0, -dhdy));

    const float inv_res = 1.0f / float(res - 1);
    const float3 pos = float3(
        -cb.world_half + float(gx) * cb.cell_size, h,
        -cb.world_half + float(gy) * cb.cell_size);
    const float2 uv = float2(float(gx) * inv_res, float(gy) * inv_res);

    const uint out_base = uint(vertex_base[uint(tile)] + slot) * 11u;
    vertices[out_base + 0u] = pos.x;
    vertices[out_base + 1u] = pos.y;
    vertices[out_base + 2u] = pos.z;
    vertices[out_base + 3u] = n.x;
    vertices[out_base + 4u] = n.y;
    vertices[out_base + 5u] = n.z;
    vertices[out_base + 6u] = uv.x;
    vertices[out_base + 7u] = uv.y;
    vertices[out_base + 8u] = water[gi];
    vertices[out_base + 9u] = sediment[gi];
    vertices[out_base + 10u] = wear[gi];
}
