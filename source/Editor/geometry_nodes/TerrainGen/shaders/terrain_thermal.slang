// Thermal (talus) erosion: material above the stable slope angle slides to
// lower neighbours. Each cell's net change follows only from the OLD heights
// of its 3x3 neighbourhood (same math as the CPU fallback in
// TerrainGen/src/Erosion.cpp), so the pass is race-free double-buffered.
// Border cells copy through unchanged.

#include "terrain_common.slangh"

ConstantBuffer<TerrainConstants> cb : register(b0);

StructuredBuffer<float> height : register(t0);

RWStructuredBuffer<float> height_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint res = uint(cb.res);
    if (dtid.x >= res * res) return;
    uint x = dtid.x % res;
    uint y = dtid.x / res;

    if (x == 0 || y == 0 || x == res - 1 || y == res - 1) {
        height_out[dtid.x] = height[dtid.x];
        return;
    }

    float center = height[dtid.x];
    float change = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float nh = height[uint(int(y + dy)) * res + uint(int(x + dx))];
            float diff = nh - center;
            float signed_excess;
            if (diff > cb.talus) {
                signed_excess = diff - cb.talus;
            }
            else if (-diff > cb.talus) {
                // center is the higher one: diff < -talus, so this is
                // already negative — the center loses material.
                signed_excess = diff + cb.talus;
            }
            else {
                signed_excess = 0.0;
            }
            change += signed_excess * cb.strength * 0.125;
        }
    }
    height_out[dtid.x] = center + change;
}
