// Particle compaction — remove dead particles, pack alive ones  (3D)
// Single-pass prefix-sum compaction.
//
// The host ZEROES ptcl_alive_out before this dispatch (see fluid.cpp). This
// shader must only WRITE, never CLEAR: a dead thread writing
// ptcl_alive_out[idx]=0 at its own idx raced with packed survivors (a dead
// particle whose index lands below the live count killed the survivor that
// compacted into that slot), and slots above the live count kept the STALE
// alive=1 flags of the ping-pong buffer's previous contents — every deposited
// particle resurrected one frame later at its pre-deposit position and
// re-deposited its mass every frame until the far+stalled kill caught it
// (mass minting; the density climb to ~600 was partly this).

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<uint> ptcl_alive : register(t0);
StructuredBuffer<float4> ptcl_pos : register(t1);
StructuredBuffer<float4> ptcl_vel : register(t2);
StructuredBuffer<float4> ptcl_color : register(t3);
RWByteAddressBuffer ptcl_counter : register(u0);
RWStructuredBuffer<float4> ptcl_pos_out : register(u1);
RWStructuredBuffer<float4> ptcl_vel_out : register(u2);
RWStructuredBuffer<float4> ptcl_color_out : register(u3);
RWStructuredBuffer<uint> ptcl_alive_out : register(u4);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) return;  // dead: write nothing (buffer pre-zeroed)

    // Allocate compacted slot
    uint slot;
    ptcl_counter.InterlockedAdd(0, 1, slot);
    slot = slot % uint(cb.max_particles);

    ptcl_pos_out[slot] = ptcl_pos[idx];
    ptcl_vel_out[slot] = ptcl_vel[idx];
    ptcl_color_out[slot] = ptcl_color[idx];
    ptcl_alive_out[slot] = 1;
}
