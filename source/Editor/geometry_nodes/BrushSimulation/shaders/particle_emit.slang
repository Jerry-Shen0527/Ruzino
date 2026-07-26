// Particle emission — Wetbrush Section 4.3  (3D)
// Mode 0: emit from bristle samples (Nb*S threads). Bristles are 2D curves in
//         the XY plane at the canvas surface; emitted particles start there and
//         jitter upward into the paint volume.
// Mode 1: emit from 3D grid cells near brush (res*res*res_z threads)

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
RWByteAddressBuffer ptcl_counter : register(u0);  // atomic counter
RWStructuredBuffer<float4> ptcl_pos : register(u1);
RWStructuredBuffer<float4> ptcl_vel : register(u2);
RWStructuredBuffer<float4> ptcl_color : register(u3);
RWStructuredBuffer<uint> ptcl_alive : register(u4);

// Mode 0: bristle sample data (sample_pos.xyz = 3D position)
StructuredBuffer<float4> sample_pos : register(t0);
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

    // Canvas surface Z (bottom of the paint volume)
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;

    if (cb.emit_mode == 0) {
        // Mode 0: emit from bristle samples with density overflow
        int total = cb.max_particles; // dispatched as num_bristles * samples_per_bristle externally
        if (idx >= total) return;

        float4 col = sample_color[idx];
        if (col.w < 0.1) return; // only emit if sample carries paint

        // Per-sample hash for a stable jitter (sin-hash, cheap GPU trick).
        uint slot = alloc_slot();
        // Emit at the sample's 3D position with a small upward jitter.
        float jitter_z = frac(sin(float(idx) * 127.1 + 0.5) * 43758.5453) * cb.cell_size;
        ptcl_pos[slot] = float4(sample_pos[idx].xyz + float3(0, 0, jitter_z), 0.0);
        ptcl_vel[slot] = float4(0, 0, 0, 0);
        ptcl_color[slot] = col;
        ptcl_alive[slot] = 1;
    }
    else {
        // Mode 1: emit from grid cells within D0 of brush (3D)
        int res = cb.grid_res;
        int res_z = cb.grid_res_z;
        if (idx >= res * res * res_z) return;

        int z = idx / (res * res);
        int rem = idx - z * res * res;
        int y = rem / res;
        int x = rem - y * res;

        float half_p = cb.paper_size * 0.5;
        float half_h = cb.height_extent * 0.5;
        float cell_z = cb.height_extent / float(cb.grid_res_z);
        float wx = (float(x) + 0.5) * cb.cell_size - half_p + cb.grid_center_x;
        float wy = (float(y) + 0.5) * cb.cell_size - half_p + cb.grid_center_y;
        float wz = (float(z) + 0.5) * cell_z - half_h + cb.grid_center_z;

        float3 cell_pos = float3(wx, wy, wz);
        float3 brush_pos3 = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z);
        // Compare in XY (brush footprint); accept any Z near the brush volume.
        float2 dxy = cell_pos.xy - brush_pos3.xy;
        float dist = length(dxy);

        if (dist > cb.brush_radius + cb.D0) return;
        if (density[idx] < 0.05) return;

        uint slot = alloc_slot();
        ptcl_pos[slot] = float4(cell_pos, 0.0);
        ptcl_vel[slot] = float4(0, 0, 0, 0);
        ptcl_color[slot] = float4(0, 0, 0, density[idx]);
        ptcl_alive[slot] = 1;
    }
}
