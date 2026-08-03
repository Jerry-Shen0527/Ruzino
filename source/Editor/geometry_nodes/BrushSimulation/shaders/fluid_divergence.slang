// Compute velocity divergence for pressure projection — 3D version
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// 3D: div = ∂u/∂x + ∂v/∂y + ∂w/∂z
// Uses central differences with Neumann BC at global grid boundaries.
//
// Global grid: neighbor access uses global coords/strides (res, res_z).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> vel_x : register(t0);
StructuredBuffer<float> vel_y : register(t1);
StructuredBuffer<float> vel_z : register(t2);
// Boundary-condition fields (Wetbrush §4.2): wetness+density marks cells
// whose paint has dried. Dried cells act as solid during pressure projection.
StructuredBuffer<float> wetness    : register(t3);
StructuredBuffer<float> density    : register(t4);
// Bristle rasterized density field (paper §4.1/§4.2: "the discretized bristle
// density and velocity fields are treated as boundary conditions in pressure
// projection"). Window-sized buffer, indexed by window-local coords.
StructuredBuffer<float> bristle_density : register(t5);

RWStructuredBuffer<float> div_out : register(u0);

// Solid in pressure projection (paper §4.2: "Once the dryness reaches a
// threshold, a cell is considered as a solid cell"). The density gate on the
// dryness test is essential: an EMPTY cell (no paint ever deposited) has
// wetness == 0 by initialization, but it is NOT solid — fluid must flow into
// empty space. Only cells that HAVE paint and have dried below the wetness
// threshold block the fluid.
bool is_solid(uint idx)
{
    return density[idx] > 1e-6f && wetness[idx] < 0.01f;
}

// Brush interior boundary (paper §4.2): a cell occupied by bristles acts as a
// no-flux wall during pressure projection, so grid liquid is deflected around
// the brush footprint rather than flowing through it. bristle_density is the
// §4.1 rasterized field (window-sized); the gate threshold is small so only
// cells genuinely under bristles count. Takes GLOBAL coords, converts to
// window-local; out-of-window cells are not brush.
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

// A cell blocks flow if it is dried-solid OR brush-interior (both no-flux).
bool is_blocked(uint idx, uint gx, uint gy, uint gz)
{
    return is_solid(idx) || is_brush_g(gx, gy, gz);
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int RES  = cb.res;
    int RESZ = cb.res_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;
    uint RNN = (uint)(RES * RES);

    // Brush interior or solid cell: treat as a wall (no divergence, no-flux
    // at the interface). Paper §4.2 boundary conditions.
    if (is_blocked(idx, gx, gy, gz)) {
        div_out[idx] = 0.0f;
        return;
    }

    float div = 0.0f;

    // ∂u/∂x — skip blocked neighbors (no-flux across solid/brush interfaces)
    if (gx > 0 && gx < (uint)RES - 1) {
        if (!is_blocked(idx - 1, gx - 1, gy, gz)) div += vel_x[idx] - vel_x[idx - 1];
        if (!is_blocked(idx + 1, gx + 1, gy, gz)) div += vel_x[idx + 1] - vel_x[idx];
    } else if (gx == 0) {
        if (!is_blocked(idx + 1, gx + 1, gy, gz)) div += vel_x[idx + 1] - vel_x[idx];
    } else {
        if (!is_blocked(idx - 1, gx - 1, gy, gz)) div += vel_x[idx] - vel_x[idx - 1];
    }

    // ∂v/∂y
    if (gy > 0 && gy < (uint)RES - 1) {
        if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz)) div += vel_y[idx] - vel_y[idx - (uint)RES];
        if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz)) div += vel_y[idx + (uint)RES] - vel_y[idx];
    } else if (gy == 0) {
        if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz)) div += vel_y[idx + (uint)RES] - vel_y[idx];
    } else {
        if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz)) div += vel_y[idx] - vel_y[idx - (uint)RES];
    }

    // ∂w/∂z
    if (RESZ > 1) {
        if (gz > 0 && gz < (uint)RESZ - 1) {
            if (!is_blocked(idx - RNN, gx, gy, gz - 1)) div += vel_z[idx] - vel_z[idx - RNN];
            if (!is_blocked(idx + RNN, gx, gy, gz + 1)) div += vel_z[idx + RNN] - vel_z[idx];
        } else if (gz == 0) {
            if (!is_blocked(idx + RNN, gx, gy, gz + 1)) div += vel_z[idx + RNN] - vel_z[idx];
        } else {
            if (!is_blocked(idx - RNN, gx, gy, gz - 1)) div += vel_z[idx] - vel_z[idx - RNN];
        }
    }

    div_out[idx] = -div;
}
