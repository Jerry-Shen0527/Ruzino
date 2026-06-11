// Semi-Lagrangian advection — Stam 1999, 3D version
// Wetbrush Section 4.2: "we advect all of the fields using the semi-Lagrangian method"
// 3D grid: res × res × res_z, with 3-component velocity (vel_x, vel_y, vel_z)

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> vel_x   : register(t1);
StructuredBuffer<float> vel_y   : register(t2);
StructuredBuffer<float> vel_z   : register(t3);

RWStructuredBuffer<float> field_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    int D = cb.res_z;
    int total = N * N * D;
    if (idx >= (uint)total) return;

    uint z = idx / (uint)(N * N);
    uint rem = idx % (uint)(N * N);
    uint y = rem / (uint)N;
    uint x = rem % (uint)N;

    float dt0 = cb.dt * (float)N;
    float dt_z = cb.dt * (float)D;

    // Trace back
    float px = (float)x - dt0 * vel_x[idx];
    float py = (float)y - dt0 * vel_y[idx];
    float pz = (float)z - dt_z * vel_z[idx];

    // Clamp to grid bounds
    float max_xy = (float)N - 1.5f;
    float max_z = (float)D - 1.5f;
    px = max(0.5f, min(px, max_xy));
    py = max(0.5f, min(py, max_xy));
    pz = max(0.5f, min(pz, max_z));

    // Trilinear interpolation
    int x0 = (int)px, y0 = (int)py, z0 = (int)pz;
    int x1 = min(x0 + 1, N - 1), y1 = min(y0 + 1, N - 1), z1 = min(z0 + 1, D - 1);
    float sx = px - (float)x0, sy = py - (float)y0, sz = pz - (float)z0;

    float c000 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z0, N, D)];
    float c100 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z0, N, D)];
    float c010 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z0, N, D)];
    float c110 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z0, N, D)];
    float c001 = field_in[grid_idx_3d((uint)x0, (uint)y0, (uint)z1, N, D)];
    float c101 = field_in[grid_idx_3d((uint)x1, (uint)y0, (uint)z1, N, D)];
    float c011 = field_in[grid_idx_3d((uint)x0, (uint)y1, (uint)z1, N, D)];
    float c111 = field_in[grid_idx_3d((uint)x1, (uint)y1, (uint)z1, N, D)];

    float c00 = lerp(c000, c100, sx);
    float c10 = lerp(c010, c110, sx);
    float c01 = lerp(c001, c101, sx);
    float c11 = lerp(c011, c111, sx);
    float c0 = lerp(c00, c10, sy);
    float c1 = lerp(c01, c11, sy);

    field_out[idx] = lerp(c0, c1, sz);
}
