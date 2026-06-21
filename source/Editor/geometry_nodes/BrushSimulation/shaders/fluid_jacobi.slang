// Jacobi iteration — 3D version
// Wetbrush Algorithm 1: fixed-point pressure projection uses Jacobi iterations
// 3D grid: res × res × res_z, 6-neighbor stencil
// Pressure: sum of 6 neighbors, divisor = 6
// Diffusion: implicit solve with dt*rate*N^2
//
// Window-relative: neighbors are addressed with WINDOW-LOCAL strides
// (WIN, WIN*WIN), and boundary checks use window-local coords/dims.

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
    int WIN  = cb.window_size_x;   // window XY size
    int WINZ = cb.window_size_z;   // window Z size
    uint WNN = (uint)(WIN * WIN);  // window Z stride (in local index space)

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Window-local coords and index.
    uint lx = c.lx, ly = c.ly, lz = c.lz;
    uint idx = c.idx;

    float center = field_in[idx];

    if (cb.jacobi_mode == 1) {
        // Pressure solve. Solid cells keep pressure = 0 (Dirichlet); fluid
        // cells average only their fluid neighbors (divisor = fluid count).
        if (is_solid(idx)) { field_out[idx] = 0.0f; return; }

        float sum = 0.0f;
        int fluid_neighbors = 0;
        if (lx > 0)              { if (!is_solid(idx - 1))    { sum += field_in[idx - 1];    fluid_neighbors++; } }
        if (lx < (uint)WIN - 1)  { if (!is_solid(idx + 1))    { sum += field_in[idx + 1];    fluid_neighbors++; } }
        if (ly > 0)              { if (!is_solid(idx - (uint)WIN)){ sum += field_in[idx - (uint)WIN]; fluid_neighbors++; } }
        if (ly < (uint)WIN - 1)  { if (!is_solid(idx + (uint)WIN)){ sum += field_in[idx + (uint)WIN]; fluid_neighbors++; } }
        if (lz > 0)              { if (!is_solid(idx - WNN))  { sum += field_in[idx - WNN];  fluid_neighbors++; } }
        if (lz < (uint)WINZ - 1) { if (!is_solid(idx + WNN))  { sum += field_in[idx + WNN];  fluid_neighbors++; } }

        // A fluid cell with NO fluid neighbors is fully enclosed by solid —
        // treat it as solid (pressure pinned to 0) instead of collapsing to
        // the raw divergence, which would inject spurious velocity.
        if (fluid_neighbors == 0) { field_out[idx] = 0.0f; return; }

        field_out[idx] = (rhs[idx] + sum) / float(fluid_neighbors);
    } else {
        // Diffusion (viscosity): standard 6-neighbor Laplacian with Neumann BC.
        float left   = (lx > 0)              ? field_in[idx - 1]        : center;
        float right  = (lx < (uint)WIN - 1)  ? field_in[idx + 1]        : center;
        float up     = (ly > 0)              ? field_in[idx - (uint)WIN] : center;
        float down   = (ly < (uint)WIN - 1)  ? field_in[idx + (uint)WIN] : center;
        float back   = (lz > 0)              ? field_in[idx - WNN]      : center;
        float front  = (lz < (uint)WINZ - 1) ? field_in[idx + WNN]      : center;
        float sum = left + right + up + down + back + front;

        float a = cb.jacobi_alpha;
        field_out[idx] = (rhs[idx] + a * sum) / (1.0f + a * 6.0f);
    }
}
