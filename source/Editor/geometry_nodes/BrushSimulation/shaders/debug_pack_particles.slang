// Debug visualization pack #1: live active-window particles -> renderer point
// cloud. Reads ptcl_pos/ptcl_color (already compacted to [0, count) by
// particle_compact) and writes the blocked debug-draw layout consumed by
// Hd_RUZINO_Points via the shared GPU buffer registry:
//
//   [0 .. 3C)   positions (C = capacity, entries >= count unused)
//   [3C .. 4C)  radii     (C * float)
//   [4C .. 7C)  colors    (C * float3, RGB)
//
// Blocks are spaced by the fixed CAPACITY (not the live count) so the renderer
// can compute offsets from the registry metadata alone. Color: ptcl_color.rgb
// is the normalized RYB pigment; convert to RGB here for direct shader use.

#include "common.slangh"

struct DebugParticlesCB {
    int count;       // live particle count (from ptcl_counter readback)
    int capacity;    // block stride (MAX_PARTICLES)
    float radius;    // world-space point radius
    float _pad0;
};

ConstantBuffer<DebugParticlesCB> cb : register(b0);

StructuredBuffer<float4> ptcl_pos : register(t0);    // xyz = world pos
StructuredBuffer<float4> ptcl_color : register(t1);  // rgb = RYB, w = mass

RWStructuredBuffer<float> debug_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint n = uint(max(cb.count, 0));
    if (tid.x >= n) return;

    float3 p = ptcl_pos[tid.x].xyz;
    float3 ryb = clamp(ptcl_color[tid.x].rgb, float3(0.0), float3(1.0));
    float3 rgb = ryb_to_rgb(ryb.x, ryb.y, ryb.z);

    uint i = tid.x;
    uint C = uint(max(cb.capacity, 1));
    debug_out[i * 3 + 0] = p.x;
    debug_out[i * 3 + 1] = p.y;
    debug_out[i * 3 + 2] = p.z;
    debug_out[3 * C + i] = cb.radius;
    uint co = 4 * C;
    debug_out[co + i * 3 + 0] = rgb.r;
    debug_out[co + i * 3 + 1] = rgb.g;
    debug_out[co + i * 3 + 2] = rgb.b;
}
