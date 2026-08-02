// Pack density + 3 color channels (4 separate float buffers) into one
// interleaved float4 buffer. Used by brush_wb_commit to produce the Float4
// paint-field buffer that the render rprim consumes (via the shared GPU buffer
// registry or the USD primvar fallback).
//
// Dispatched with one thread per global grid cell (res * res * res_z).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);

RWStructuredBuffer<float4> packed_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint total = uint(cb.res) * uint(cb.res) * uint(cb.res_z);
    if (tid.x >= total) return;

    packed_out[tid.x] = float4(
        density[tid.x],
        color_r[tid.x],
        color_y[tid.x],
        color_b[tid.x]);
}
