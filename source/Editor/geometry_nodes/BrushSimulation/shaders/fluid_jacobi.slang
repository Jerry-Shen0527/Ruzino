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
// Rasterized swarm density (§4.1/§4.3 joint liquid field). Window-sized.
StructuredBuffer<float> ptcl_density : register(t5);

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

// LIQUID OCCUPANCY (§4.1/§4.3 joint field): grid paint + rasterized swarm +
// bristles. No liquid → AIR = free surface with Dirichlet p = 0. The
// projection must not enforce incompressibility of empty space (see
// fluid_divergence.slang's is_air note).
bool is_air(uint idx, uint gx, uint gy, uint gz)
{
    if (is_brush_g(gx, gy, gz)) return false;
    if (density[idx] > 1e-6f) return false;
    int w = win_field_idx(
        int(gx), int(gy), int(gz), cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    if (w >= 0 && ptcl_density[w] > 1e-6f) return false;
    return true;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int RES  = cb.res;            // global XY size
    int RESZ = cb.res_z;          // global Z size
    uint RNN = (uint)(RES * RES); // global Z stride

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    // Global coords and index (BC fields are GLOBAL canvas state).
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;
    int WSX = cb.window_size_x;
    int WSZ = cb.window_size_z;
    // Window-local index (field_in/field_out/rhs are WINDOW-SIZED).
    int w0 = wnb(c, 0, 0, 0, WSX, WSZ);

    if (cb.jacobi_mode == 1) {
        // Pressure solve with Neumann BC at solid interfaces: solid cells do
        // not participate (their pressure is never read by fluid cells, which
        // skip solid neighbors below). Fluid cells average only their fluid
        // neighbors and divide by the fluid-neighbor count. This is equivalent
        // to mirroring the fluid cell's own pressure at solid boundaries
        // (∂P/∂n = 0), so the fluid is blocked by solid cells rather than
        // sucked toward them. See fluid_gradient.slang for the matching BC in
        // the pressure-gradient step. Out-of-window neighbors are skipped the
        // same way (no-flux at the window edge, far from the brush).
        if (is_blocked(idx, gx, gy, gz)) { field_out[w0] = 0.0f; return; }
        // AIR = free surface: Dirichlet p = 0 (the projection solves the
        // LIQUID, not empty space).
        if (is_air(idx, gx, gy, gz)) { field_out[w0] = 0.0f; return; }

        float sum = 0.0f;
        int fluid_neighbors = 0;
        int wxm = wnb(c, -1, 0, 0, WSX, WSZ), wxp = wnb(c, 1, 0, 0, WSX, WSZ);
        int wym = wnb(c, 0, -1, 0, WSX, WSZ), wyp = wnb(c, 0, 1, 0, WSX, WSZ);
        int wzm = wnb(c, 0, 0, -1, WSX, WSZ), wzp = wnb(c, 0, 0, 1, WSX, WSZ);
        // An AIR neighbor is a free surface: it participates in the average
        // with Dirichlet p = 0 (counted, contributes nothing). A blocked
        // (solid/brush) neighbor is excluded (Neumann mirror).
        if (gx > 0)              { if (is_air(idx - 1, gx - 1, gy, gz) && wxm >= 0) { fluid_neighbors++; } else if (!is_blocked(idx - 1, gx - 1, gy, gz) && wxm >= 0)    { sum += field_in[wxm];    fluid_neighbors++; } }
        if (gx < (uint)RES - 1)  { if (is_air(idx + 1, gx + 1, gy, gz) && wxp >= 0) { fluid_neighbors++; } else if (!is_blocked(idx + 1, gx + 1, gy, gz) && wxp >= 0)    { sum += field_in[wxp];    fluid_neighbors++; } }
        if (gy > 0)              { if (is_air(idx - (uint)RES, gx, gy - 1, gz) && wym >= 0) { fluid_neighbors++; } else if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz) && wym >= 0) { sum += field_in[wym]; fluid_neighbors++; } }
        if (gy < (uint)RES - 1)  { if (is_air(idx + (uint)RES, gx, gy + 1, gz) && wyp >= 0) { fluid_neighbors++; } else if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz) && wyp >= 0) { sum += field_in[wyp]; fluid_neighbors++; } }
        if (gz > 0)              { if (is_air(idx - RNN, gx, gy, gz - 1) && wzm >= 0)  { fluid_neighbors++; } else if (!is_blocked(idx - RNN, gx, gy, gz - 1) && wzm >= 0)  { sum += field_in[wzm];    fluid_neighbors++; } }
        if (gz < (uint)RESZ - 1) { if (is_air(idx + RNN, gx, gy, gz + 1) && wzp >= 0)  { fluid_neighbors++; } else if (!is_blocked(idx + RNN, gx, gy, gz + 1) && wzp >= 0)  { sum += field_in[wzp];    fluid_neighbors++; } }

        // A fluid cell with NO fluid neighbors is fully enclosed by solid —
        // treat it as solid (pressure pinned to 0) instead of collapsing to
        // the raw divergence, which would inject spurious velocity.
        if (fluid_neighbors == 0) { field_out[w0] = 0.0f; return; }

        field_out[w0] = (rhs[w0] + sum) / float(fluid_neighbors);
    } else {
        // Diffusion (viscosity): standard 6-neighbor Laplacian with Neumann BC
        // at the global grid edges AND at the window edge (mirror the center).
        float left   = (gx > 0)              ? ((wnb(c, -1, 0, 0, WSX, WSZ) >= 0) ? field_in[wnb(c, -1, 0, 0, WSX, WSZ)] : field_in[w0]) : field_in[w0];
        float right  = (gx < (uint)RES - 1)  ? ((wnb(c, 1, 0, 0, WSX, WSZ) >= 0)  ? field_in[wnb(c, 1, 0, 0, WSX, WSZ)]  : field_in[w0]) : field_in[w0];
        float up     = (gy > 0)              ? ((wnb(c, 0, -1, 0, WSX, WSZ) >= 0) ? field_in[wnb(c, 0, -1, 0, WSX, WSZ)] : field_in[w0]) : field_in[w0];
        float down   = (gy < (uint)RES - 1)  ? ((wnb(c, 0, 1, 0, WSX, WSZ) >= 0)  ? field_in[wnb(c, 0, 1, 0, WSX, WSZ)]  : field_in[w0]) : field_in[w0];
        float back   = (gz > 0)              ? ((wnb(c, 0, 0, -1, WSX, WSZ) >= 0) ? field_in[wnb(c, 0, 0, -1, WSX, WSZ)] : field_in[w0]) : field_in[w0];
        float front  = (gz < (uint)RESZ - 1) ? ((wnb(c, 0, 0, 1, WSX, WSZ) >= 0)  ? field_in[wnb(c, 0, 0, 1, WSX, WSZ)]  : field_in[w0]) : field_in[w0];
        float sum = left + right + up + down + back + front;

        float a = cb.jacobi_alpha;
        field_out[w0] = (rhs[w0] + a * sum) / (1.0f + a * 6.0f);
    }
}
