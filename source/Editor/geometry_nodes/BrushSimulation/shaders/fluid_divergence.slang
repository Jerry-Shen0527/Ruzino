// Compute velocity divergence for pressure projection
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// Stam convention: div = -0.5 * h * (u[i+1]-u[i-1] + v[j+1]-v[j-1])

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> vel_x : register(t0);
StructuredBuffer<float> vel_y : register(t1);

RWStructuredBuffer<float> div_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    if (idx >= (uint)N * N) return;

    uint x = idx % (uint)N;
    uint y = idx / (uint)N;

    float div = 0.0f;

    // ∂u/∂x with Neumann boundary (zero velocity at edges)
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

    div_out[idx] = -div * 0.5f;
}
