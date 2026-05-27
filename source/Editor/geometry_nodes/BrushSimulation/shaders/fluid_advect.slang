// Semi-Lagrangian advection — Stam 1999
// Wetbrush Section 4.2: "we advect all of the fields using the semi-Lagrangian method"

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> vel_x   : register(t1);
StructuredBuffer<float> vel_y   : register(t2);

RWStructuredBuffer<float> field_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    if (idx >= (uint)N * N) return;

    uint x = idx % (uint)N;
    uint y = idx / (uint)N;

    float dt0 = cb.dt * (float)N;
    float max_c = (float)N - 1.5f;

    // Trace back in Stam convention: velocity is in sim units
    float px = (float)x - dt0 * vel_x[idx];
    float py = (float)y - dt0 * vel_y[idx];

    // Clamp to grid bounds
    px = max(0.5f, min(px, max_c));
    py = max(0.5f, min(py, max_c));

    // Bilinear interpolation
    int x0 = (int)px;
    int y0 = (int)py;
    int x1 = min(x0 + 1, N - 1);
    int y1 = min(y0 + 1, N - 1);
    float sx = px - (float)x0;
    float sy = py - (float)y0;

    field_out[idx] =
        (1.0f - sx) * (1.0f - sy) * field_in[grid_idx((uint)x0, (uint)y0, N)] +
        sx * (1.0f - sy) * field_in[grid_idx((uint)x1, (uint)y0, N)] +
        (1.0f - sx) * sy * field_in[grid_idx((uint)x0, (uint)y1, N)] +
        sx * sy * field_in[grid_idx((uint)x1, (uint)y1, N)];
}
