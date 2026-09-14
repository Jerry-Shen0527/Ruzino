// 2:1 balance relaxation: after this converges (max_level+1 iterations of
// ping-pong, host-driven), no tile's level exceeds a neighbor's by more
// than 1, which is exactly the condition the edge stitching assumes.

#include "terrain_ada_common.slangh"

ConstantBuffer<AdaConstants> cb : register(b0);

StructuredBuffer<float> level_in : register(t0);

RWStructuredBuffer<float> level_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const int T = cb.tiles;
    const int tile = int(dtid.x);
    if (tile >= T * T) return;

    const float own = level_in[uint(tile)];
    const int tx = tile % T;
    const int ty = tile / T;

    float best_neighbor = -1e30f;
    if (tx > 0) best_neighbor = max(best_neighbor, level_in[uint(tile - 1)]);
    if (tx < T - 1)
        best_neighbor = max(best_neighbor, level_in[uint(tile + 1)]);
    if (ty > 0)
        best_neighbor = max(best_neighbor, level_in[uint(tile - T)]);
    if (ty < T - 1)
        best_neighbor = max(best_neighbor, level_in[uint(tile + T)]);

    float result = own;
    if (best_neighbor > -1e29f) {
        result = max(result, best_neighbor - 1.0f);
    }
    level_out[uint(tile)] = clamp(
        result, 0.0f, float(cb.max_level));
}
