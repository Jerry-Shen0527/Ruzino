// Debug visualization pack #3: brush bristle chains -> renderer capsule
// segments. One thread per segment (vertex v -> v+1 of bristle b); reads
// bristle_data (10-vertex chains) and the CURRENT sample_liquid (post
// ping-pong) so each segment is colored by the liquid its midpoint carries:
// dry = neutral gray, loaded = pigment color.
//
// Blocked SEGMENT layout (C = capacity = Nb * (M-1)):
//   [0 .. 3C)   endpoints A (C * float3)
//   [3C .. 6C)  endpoints B (C * float3)
//   [6C .. 7C)  radii     (C * float)
//   [7C .. 10C) colors    (C * float3, RGB)

#include "common.slangh"

#define BRV_STRIDE 2  // float4s per bristle vertex in bristle_data (pos, vel)

struct DebugBristlesCB {
    int num_bristles;        // Nb
    int verts_per_bristle;   // M
    int samples_per_bristle; // S
    float radius;            // capsule radius (fraction of brush radius)
};

ConstantBuffer<DebugBristlesCB> cb : register(b0);

StructuredBuffer<float4> bristle_data : register(t0);          // Nb * M * 2
StructuredBuffer<SampleLiquid> sample_liquid : register(t1);   // Nb * S (current)

RWStructuredBuffer<float> debug_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int segs_per = cb.verts_per_bristle - 1;
    int total = cb.num_bristles * segs_per;
    if (int(tid.x) >= total) return;

    int b = int(tid.x) / segs_per;
    int v = int(tid.x) % segs_per;

    float3 a = bristle_data[(b * cb.verts_per_bristle + v) * BRV_STRIDE].xyz;
    float3 c = bristle_data[(b * cb.verts_per_bristle + v + 1) * BRV_STRIDE].xyz;

    // Nearest resampled sample of the segment midpoint carries the liquid.
    float t = (float(v) + 0.5) / float(cb.verts_per_bristle);
    int s = clamp(int(t * cb.samples_per_bristle), 0, cb.samples_per_bristle - 1);
    SampleLiquid liquid = sample_liquid[b * cb.samples_per_bristle + s];

    // FIXED BLUE reference color (user request): the brush must be readable
    // against the paint. The previous liquid-load coloring rendered a dipped
    // brush black-on-black — invisible next to the dark paint it deposits.
    float3 col = float3(0.25, 0.5, 1.0);

    uint i = tid.x;
    uint C = uint(cb.num_bristles * segs_per);
    debug_out[i * 3 + 0] = a.x;
    debug_out[i * 3 + 1] = a.y;
    debug_out[i * 3 + 2] = a.z;
    uint bo = 3 * C;
    debug_out[bo + i * 3 + 0] = c.x;
    debug_out[bo + i * 3 + 1] = c.y;
    debug_out[bo + i * 3 + 2] = c.z;
    debug_out[6 * C + i] = cb.radius;
    uint co = 7 * C;
    debug_out[co + i * 3 + 0] = col.r;
    debug_out[co + i * 3 + 1] = col.g;
    debug_out[co + i * 3 + 2] = col.b;
}
