// Jacobi iteration — 3D version
// Wetbrush Algorithm 1: fixed-point pressure projection uses Jacobi iterations
// 3D grid: res × res × res_z, 6-neighbor stencil
// Pressure: sum of 6 neighbors, divisor = 6
// Diffusion: implicit solve with dt*rate*N^2

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> rhs     : register(t1);
// Boundary-condition field for pressure projection (Wetbrush §4.2):
// solid cells get a fixed (Dirichlet) pressure of 0.
StructuredBuffer<float> bristle_psi : register(t2);
StructuredBuffer<float> wetness    : register(t3);

RWStructuredBuffer<float> field_out : register(u0);

bool is_solid(uint idx)
{
    return bristle_psi[idx] > 0.5 || wetness[idx] < 0.01;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    int D = cb.res_z;
    uint NN = (uint)(N * N);

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint x = c.x, y = c.y, z = c.z, idx = c.idx;

    float center = field_in[idx];

    if (cb.jacobi_mode == 1) {
        // Pressure solve. Solid cells keep pressure = 0 (Dirichlet); fluid
        // cells average only their fluid neighbors (divisor = fluid count).
        if (is_solid(idx)) { field_out[idx] = 0.0f; return; }

        float sum = 0.0f;
        int fluid_neighbors = 0;
        if (x > 0)           { if (!is_solid(idx - 1))     { sum += field_in[idx - 1];     fluid_neighbors++; } }
        if (x < (uint)N - 1) { if (!is_solid(idx + 1))     { sum += field_in[idx + 1];     fluid_neighbors++; } }
        if (y > 0)           { if (!is_solid(idx - (uint)N)){ sum += field_in[idx - (uint)N]; fluid_neighbors++; } }
        if (y < (uint)N - 1) { if (!is_solid(idx + (uint)N)){ sum += field_in[idx + (uint)N]; fluid_neighbors++; } }
        if (z > 0)           { if (!is_solid(idx - NN))    { sum += field_in[idx - NN];    fluid_neighbors++; } }
        if (z < (uint)D - 1) { if (!is_solid(idx + NN))    { sum += field_in[idx + NN];    fluid_neighbors++; } }

        // A fluid cell with NO fluid neighbors is fully enclosed by solid —
        // treat it as solid (pressure pinned to 0) instead of collapsing to
        // the raw divergence, which would inject spurious velocity.
        if (fluid_neighbors == 0) { field_out[idx] = 0.0f; return; }

        field_out[idx] = (rhs[idx] + sum) / float(fluid_neighbors);
    } else {
        // Diffusion (viscosity): standard 6-neighbor Laplacian with Neumann BC.
        float left   = (x > 0)           ? field_in[idx - 1]  : center;
        float right  = (x < (uint)N - 1) ? field_in[idx + 1]  : center;
        float up     = (y > 0)           ? field_in[idx - (uint)N] : center;
        float down   = (y < (uint)N - 1) ? field_in[idx + (uint)N] : center;
        float back   = (z > 0)           ? field_in[idx - NN] : center;
        float front  = (z < (uint)D - 1) ? field_in[idx + NN] : center;
        float sum = left + right + up + down + back + front;

        float a = cb.jacobi_alpha;
        field_out[idx] = (rhs[idx] + a * sum) / (1.0f + a * 6.0f);
    }
}
