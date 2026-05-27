// Jacobi iteration — used for both implicit diffusion and pressure solve
// Wetbrush Algorithm 1: fixed-point pressure projection uses Jacobi iterations
// Diffusion: implicit solve, a = dt * rate * N^2
// Pressure:  Laplacian solve, result = (div + sum_neighbors) / 4

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);  // Pressure or field to diffuse
StructuredBuffer<float> rhs     : register(t1);  // Divergence (pressure) or field center (diffuse)

RWStructuredBuffer<float> field_out : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int N = cb.res;
    if (idx >= (uint)N * N) return;

    uint x = idx % (uint)N;
    uint y = idx / (uint)N;

    // Neumann (zero-gradient) boundary: ghost cell = center value
    float center = field_in[idx];
    float left   = (x > 0)        ? field_in[idx - 1]  : center;
    float right  = (x < (uint)N-1) ? field_in[idx + 1]  : center;
    float up     = (y > 0)        ? field_in[idx - (uint)N] : center;
    float down   = (y < (uint)N-1) ? field_in[idx + (uint)N] : center;
    float sum = left + right + up + down;

    if (cb.jacobi_mode == 1) {
        // Pressure solve: P_new = (div + sum_P) / 4
        // Neumann BC: 4 neighbors always (ghost = center already counted)
        field_out[idx] = (rhs[idx] + sum) * 0.25f;
    } else {
        // Diffusion: field_new = (field_center + a * sum) / (1 + a * 4)
        float a = cb.jacobi_alpha;
        field_out[idx] = (rhs[idx] + a * sum) / (1.0f + a * 4.0f);
    }
}
