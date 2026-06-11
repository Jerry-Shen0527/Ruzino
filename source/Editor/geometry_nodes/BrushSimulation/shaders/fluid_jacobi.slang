// Jacobi iteration — 3D version
// Wetbrush Algorithm 1: fixed-point pressure projection uses Jacobi iterations
// 3D grid: res × res × res_z, 6-neighbor stencil
// Pressure: sum of 6 neighbors, divisor = 6
// Diffusion: implicit solve with dt*rate*N^2

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> rhs     : register(t1);

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

    float center = field_in[idx];

    // 6-neighbor stencil with Neumann boundary
    float left   = (x > 0)           ? field_in[idx - 1]         : center;
    float right  = (x < (uint)N - 1) ? field_in[idx + 1]         : center;
    float up     = (y > 0)           ? field_in[idx - (uint)N]   : center;
    float down   = (y < (uint)N - 1) ? field_in[idx + (uint)N]   : center;
    float back   = (z > 0)           ? field_in[idx - (uint)(N*N)]: center;
    float front  = (z < (uint)D - 1) ? field_in[idx + (uint)(N*N)]: center;
    float sum = left + right + up + down + back + front;

    if (cb.jacobi_mode == 1) {
        // Pressure solve: P_new = (div + sum_P) / 6
        field_out[idx] = (rhs[idx] + sum) / 6.0f;
    } else {
        // Diffusion: field_new = (field_center + a * sum) / (1 + a * 6)
        float a = cb.jacobi_alpha;
        field_out[idx] = (rhs[idx] + a * sum) / (1.0f + a * 6.0f);
    }
}
