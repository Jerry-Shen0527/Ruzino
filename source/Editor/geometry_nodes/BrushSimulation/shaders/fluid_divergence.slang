// Compute velocity divergence for pressure projection — 3D version
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// 3D: div = ∂u/∂x + ∂v/∂y + ∂w/∂z
// Uses central differences with Neumann BC at boundaries.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> vel_x : register(t0);
StructuredBuffer<float> vel_y : register(t1);
StructuredBuffer<float> vel_z : register(t2);

RWStructuredBuffer<float> div_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    int D = cb.res_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint x = c.x, y = c.y, z = c.z, idx = c.idx;
    uint NN = (uint)(N * N);

    float div = 0.0f;

    // ∂u/∂x
    if (x > 0 && x < (uint)N - 1)
        div += vel_x[idx + 1] - vel_x[idx - 1];
    else if (x == 0)
        div += vel_x[idx + 1];
    else
        div -= vel_x[idx - 1];

    // ∂v/∂y
    if (y > 0 && y < (uint)N - 1)
        div += vel_y[idx + (uint)N] - vel_y[idx - (uint)N];
    else if (y == 0)
        div += vel_y[idx + (uint)N];
    else
        div -= vel_y[idx - (uint)N];

    // ∂w/∂z
    if (D > 1) {
        if (z > 0 && z < (uint)D - 1)
            div += vel_z[idx + NN] - vel_z[idx - NN];
        else if (z == 0)
            div += vel_z[idx + NN];
        else
            div -= vel_z[idx - NN];
    }

    div_out[idx] = -div * 0.5f;
}
