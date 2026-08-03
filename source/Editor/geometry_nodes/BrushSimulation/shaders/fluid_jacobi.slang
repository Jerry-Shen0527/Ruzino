// Jacobi iteration — 3D version
// Wetbrush Algorithm 1: fixed-point pressure projection uses Jacobi iterations
// 3D grid: res × res × res_z, 6-neighbor stencil
// Pressure: sum of 6 neighbors, divisor = 6
// Diffusion: implicit solve with dt*rate*N^2
//
// Global grid: neighbors are addressed with GLOBAL strides (res, res*res),
// and boundary checks use global coords/dims.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);
StructuredBuffer<float> rhs     : register(t1);
// Boundary-condition fields for pressure projection (Wetbrush §4.2):
// solid cells (dried paint) are excluded from the pressure average —
// equivalent to a Neumann BC (∂P/∂n = 0) at solid interfaces. Empty cells
// (no paint) are NOT solid even though their wetness is 0 by initialization;
// the density gate enforces that.
StructuredBuffer<float> wetness    : register(t2);
StructuredBuffer<float> density    : register(t3);
// Bristle rasterized density (paper §4.2 boundary condition). Window-sized.
StructuredBuffer<float> bristle_density : register(t4);

RWStructuredBuffer<float> field_out : register(u0);

bool is_solid(uint idx)
{
    return density[idx] > 1e-6f && wetness[idx] < 0.01f;
}

// Brush interior boundary (paper §4.2): bristle-occupied cells act as no-flux
// walls in pressure projection. Global coords -> window-local lookup.
bool is_brush_g(uint gx, uint gy, uint gz)
{
    int lx = int(gx) - cb.window_origin_x;
    int ly = int(gy) - cb.window_origin_y;
    int lz = int(gz) - cb.window_origin_z;
    int WSX = cb.window_size_x;
    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX || lz < 0 || lz >= cb.window_size_z)
        return false;
    uint gi = uint(lz) * uint(WSX * WSX) + uint(ly) * uint(WSX) + uint(lx);
    return bristle_density[gi] > cb.brush_boundary_gate;
}

bool is_blocked(uint idx, uint gx, uint gy, uint gz)
{
    return is_solid(idx) || is_brush_g(gx, gy, gz);
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int RES  = cb.res;            // global XY size
    int RESZ = cb.res_z;          // global Z size
    uint RNN = (uint)(RES * RES); // global Z stride

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Global coords and index.
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;

    float center = field_in[idx];

    if (cb.jacobi_mode == 1) {
        // Pressure solve with Neumann BC at solid interfaces: solid cells do
        // not participate (their pressure is never read by fluid cells, which
        // skip solid neighbors below). Fluid cells average only their fluid
        // neighbors and divide by the fluid-neighbor count. This is equivalent
        // to mirroring the fluid cell's own pressure at solid boundaries
        // (∂P/∂n = 0), so the fluid is blocked by solid cells rather than
        // sucked toward them. See fluid_gradient.slang for the matching BC in
        // the pressure-gradient step.
        if (is_blocked(idx, gx, gy, gz)) { field_out[idx] = 0.0f; return; }

        float sum = 0.0f;
        int fluid_neighbors = 0;
        if (gx > 0)              { if (!is_blocked(idx - 1, gx - 1, gy, gz))    { sum += field_in[idx - 1];    fluid_neighbors++; } }
        if (gx < (uint)RES - 1)  { if (!is_blocked(idx + 1, gx + 1, gy, gz))    { sum += field_in[idx + 1];    fluid_neighbors++; } }
        if (gy > 0)              { if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz)){ sum += field_in[idx - (uint)RES]; fluid_neighbors++; } }
        if (gy < (uint)RES - 1)  { if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz)){ sum += field_in[idx + (uint)RES]; fluid_neighbors++; } }
        if (gz > 0)              { if (!is_blocked(idx - RNN, gx, gy, gz - 1))  { sum += field_in[idx - RNN];  fluid_neighbors++; } }
        if (gz < (uint)RESZ - 1) { if (!is_blocked(idx + RNN, gx, gy, gz + 1))  { sum += field_in[idx + RNN];  fluid_neighbors++; } }

        // A fluid cell with NO fluid neighbors is fully enclosed by solid —
        // treat it as solid (pressure pinned to 0) instead of collapsing to
        // the raw divergence, which would inject spurious velocity.
        if (fluid_neighbors == 0) { field_out[idx] = 0.0f; return; }

        field_out[idx] = (rhs[idx] + sum) / float(fluid_neighbors);
    } else {
        // Diffusion (viscosity): standard 6-neighbor Laplacian with Neumann BC.
        float left   = (gx > 0)              ? field_in[idx - 1]        : center;
        float right  = (gx < (uint)RES - 1)  ? field_in[idx + 1]        : center;
        float up     = (gy > 0)              ? field_in[idx - (uint)RES] : center;
        float down   = (gy < (uint)RES - 1)  ? field_in[idx + (uint)RES] : center;
        float back   = (gz > 0)              ? field_in[idx - RNN]      : center;
        float front  = (gz < (uint)RESZ - 1) ? field_in[idx + RNN]      : center;
        float sum = left + right + up + down + back + front;

        float a = cb.jacobi_alpha;
        field_out[idx] = (rhs[idx] + a * sum) / (1.0f + a * 6.0f);
    }
}
