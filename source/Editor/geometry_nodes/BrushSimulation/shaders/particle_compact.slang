// Particle compaction — remove dead particles, pack alive ones
// Single-pass prefix-sum compaction

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<uint> ptcl_alive : register(t0);
StructuredBuffer<float2> ptcl_pos : register(t1);
StructuredBuffer<float2> ptcl_vel : register(t2);
StructuredBuffer<float4> ptcl_color : register(t3);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float2> ptcl_pos_out : register(u1);
RWStructuredBuffer<float2> ptcl_vel_out : register(u2);
RWStructuredBuffer<float4> ptcl_color_out : register(u3);
RWStructuredBuffer<uint> ptcl_alive_out : register(u4);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;

    if (ptcl_alive[idx] == 0) {
        ptcl_alive_out[idx] = 0;
        return;
    }

    // Allocate compacted slot
    uint slot;
    ptcl_counter.InterlockedAdd(0, 1, slot);
    slot = slot % uint(cb.max_particles);

    ptcl_pos_out[slot] = ptcl_pos[idx];
    ptcl_vel_out[slot] = ptcl_vel[idx];
    ptcl_color_out[slot] = ptcl_color[idx];
    ptcl_alive_out[slot] = 1;
}
