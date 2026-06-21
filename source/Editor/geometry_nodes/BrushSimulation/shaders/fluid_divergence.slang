// Compute velocity divergence for pressure projection — 3D version
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// 3D: div = ∂u/∂x + ∂v/∂y + ∂w/∂z
// Uses central differences with Neumann BC at boundaries.
//
// Window-relative: neighbor access uses window-local coords/strides.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> vel_x : register(t0);
StructuredBuffer<float> vel_y : register(t1);
StructuredBuffer<float> vel_z : register(t2);
// Boundary-condition fields (Wetbrush §4.2): bristle density ψ and wetness
// mark cells that act as solid during pressure projection.
StructuredBuffer<float> bristle_psi : register(t3);
StructuredBuffer<float> wetness    : register(t4);

RWStructuredBuffer<float> div_out : register(u0);

// A cell is treated as solid if it carries bristle mass (ψ > ψ_thr) or is
// dry (wetness < w_thr), per the paper: "treat it as a solid cell in
// pressure projection".
bool is_solid(uint idx)
{
    return bristle_psi[idx] > 0.5 || wetness[idx] < 0.01;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int WIN  = cb.window_size_x;
    int WINZ = cb.window_size_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint lx = c.lx, ly = c.ly, lz = c.lz;
    uint idx = c.idx;
    uint WNN = (uint)(WIN * WIN);

    if (is_solid(idx)) {
        // Solid cell: no divergence contribution (acts as wall).
        div_out[idx] = 0.0f;
        return;
    }

    float div = 0.0f;

    // ∂u/∂x — skip solid neighbors (no-flux across solid interfaces)
    if (lx > 0 && lx < (uint)WIN - 1) {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else if (lx == 0) {
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
    }

    // ∂v/∂y
    if (ly > 0 && ly < (uint)WIN - 1) {
        if (!is_solid(idx - (uint)WIN)) div += vel_y[idx] - vel_y[idx - (uint)WIN];
        if (!is_solid(idx + (uint)WIN)) div += vel_y[idx + (uint)WIN] - vel_y[idx];
    } else if (ly == 0) {
        if (!is_solid(idx + (uint)WIN)) div += vel_y[idx + (uint)WIN] - vel_y[idx];
    } else {
        if (!is_solid(idx - (uint)WIN)) div += vel_y[idx] - vel_y[idx - (uint)WIN];
    }

    // ∂w/∂z
    if (WINZ > 1) {
        if (lz > 0 && lz < (uint)WINZ - 1) {
            if (!is_solid(idx - WNN)) div += vel_z[idx] - vel_z[idx - WNN];
            if (!is_solid(idx + WNN)) div += vel_z[idx + WNN] - vel_z[idx];
        } else if (lz == 0) {
            if (!is_solid(idx + WNN)) div += vel_z[idx + WNN] - vel_z[idx];
        } else {
            if (!is_solid(idx - WNN)) div += vel_z[idx] - vel_z[idx - WNN];
        }
    }

    div_out[idx] = -div;
}
