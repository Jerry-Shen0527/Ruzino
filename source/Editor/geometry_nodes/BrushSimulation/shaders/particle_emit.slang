// Particle emission — Wetbrush Section 4.3
// Mode 0: emit from bristle samples (Nb*S threads)
// Mode 1: emit from grid cells near brush (N*N threads)

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
RWByteAddressBuffer ptcl_counter : register(u0);  // atomic counter
RWStructuredBuffer<float2> ptcl_pos : register(u1);
RWStructuredBuffer<float2> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);

// Mode 0: bristle sample data
StructuredBuffer<float2> sample_pos : register(t0);
StructuredBuffer<float4> sample_color : register(t1);

// Mode 1: grid data
StructuredBuffer<float> density : register(t2);

uint alloc_slot()
{
    uint orig;
    ptcl_counter.InterlockedAdd(0, 1, orig);
    return orig % uint(cb.max_particles);
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);

    if (cb.emit_mode == 0) {
        // Mode 0: emit from bristle samples with density overflow
        int total = cb.max_particles; // dispatched as num_bristles * samples_per_bristle externally
        if (idx >= total) return;

        float4 col = sample_color[idx];
        if (col.w < 0.5) return; // only emit if density is high

        uint slot = alloc_slot();
        ptcl_pos[slot] = sample_pos[idx];
        ptcl_vel[slot] = float2(0, 0);
        ptcl_color[slot] = col;
        ptcl_alive[slot] = 1;
    }
    else {
        // Mode 1: emit from grid cells within D0 of brush
        int res = cb.grid_res;
        if (idx >= res * res) return;

        int x = idx % res;
        int y = idx / res;
        float half_p = cb.paper_size * 0.5;
        float wx = (float(x) + 0.5) * cb.cell_size - half_p + cb.grid_center_x;
        float wy = (float(y) + 0.5) * cb.cell_size - half_p + cb.grid_center_y;

        float2 cell_pos = float2(wx, wy);
        float2 brush_pos2 = float2(cb.brush_pos_x, cb.brush_pos_y);
        float dist = length(cell_pos - brush_pos2);

        if (dist > cb.brush_radius + cb.D0) return;
        if (density[idx] < 0.3) return;

        uint slot = alloc_slot();
        ptcl_pos[slot] = cell_pos;
        ptcl_vel[slot] = float2(0, 0);
        ptcl_color[slot] = float4(0, 0, 0, density[idx]);
        ptcl_alive[slot] = 1;
    }
}
