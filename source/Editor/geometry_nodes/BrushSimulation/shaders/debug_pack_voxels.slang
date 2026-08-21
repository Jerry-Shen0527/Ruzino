// Debug visualization pack #2: non-zero paint grid voxels -> renderer point
// cloud (GPU compaction, no CPU readback). One thread per global grid cell;
// cells with density > eps append a point via an atomic counter (capped at
// max_out; overflow cells are dropped).
//
// Same blocked layout as debug_pack_particles:
//   [0 .. 3N)  positions   [3N .. 4N) radii   [4N .. 7N) colors (RGB)
//
// World mapping matches node_brush_wb_commit.cpp's Paint Field 3D output and
// PaintFieldMeta.gridMin exactly.

#include "common.slangh"

struct DebugVoxelsCB {
    int res;         // grid XY resolution
    int res_z;       // grid Z resolution
    float cell_sz;   // XY cell size (paper / res)
    float paper;     // paper size
    float cx;        // grid center x
    float cy;        // grid center y
    float z_floor;   // grid_min_z (= canvas z)
    float cell_z;    // Z cell size (grid_height / res_z)
    float radius;    // world-space point radius (~0.6 cell)
    float eps;       // density threshold ("non-zero")
    int max_out;     // capacity cap
    float _pad;
};

ConstantBuffer<DebugVoxelsCB> cb : register(b0);

StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);  // premultiplied RYB
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);

RWStructuredBuffer<float> debug_out : register(u0);
RWStructuredBuffer<uint> counter : register(u1);  // must be zeroed before dispatch

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint total = uint(cb.res) * uint(cb.res) * uint(cb.res_z);
    if (tid.x >= total) return;

    float d = density[tid.x];
    if (d <= cb.eps) return;

    uint slot;
    InterlockedAdd(counter[0], 1u, slot);
    if (slot >= uint(cb.max_out)) return;

    // Recover global (x, y, z) from the linear index (matches grid_idx_3d).
    uint N = uint(cb.res);
    uint x = tid.x % N;
    uint y = (tid.x / N) % N;
    uint z = tid.x / (N * N);

    float3 ryb = float3(color_r[tid.x], color_y[tid.x], color_b[tid.x]) / d;
    ryb = clamp(ryb, float3(0.0), float3(1.0));
    float3 rgb = ryb_to_rgb(ryb.x, ryb.y, ryb.z);

    float3 p;
    p.x = (x + 0.5) * cb.cell_sz - cb.paper * 0.5 + cb.cx;
    p.y = (y + 0.5) * cb.cell_sz - cb.paper * 0.5 + cb.cy;
    p.z = (z + 0.5) * cb.cell_z + cb.z_floor;

    // Blocked layout offsets depend on the CAP, not the live count: the
    // renderer cannot know the compacted count before building its MeshDesc,
    // so blocks are always spaced by max_out entries.
    uint M = uint(cb.max_out);
    debug_out[slot * 3 + 0] = p.x;
    debug_out[slot * 3 + 1] = p.y;
    debug_out[slot * 3 + 2] = p.z;
    debug_out[3 * M + slot] = cb.radius;
    uint co = 4 * M;
    debug_out[co + slot * 3 + 0] = rgb.r;
    debug_out[co + slot * 3 + 1] = rgb.g;
    debug_out[co + slot * 3 + 2] = rgb.b;
}
