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
    int RES  = cb.res;
    int RESZ = cb.res_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;
    uint RNN = (uint)(RES * RES);

    if (is_solid(idx)) {
        // Solid cell: no divergence contribution (acts as wall).
        div_out[idx] = 0.0f;
        return;
    }

    float div = 0.0f;

    // ∂u/∂x — skip solid neighbors (no-flux across solid interfaces)
    if (gx > 0 && gx < (uint)RES - 1) {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else if (gx == 0) {
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
    }

    // ∂v/∂y
    if (gy > 0 && gy < (uint)RES - 1) {
        if (!is_solid(idx - (uint)RES)) div += vel_y[idx] - vel_y[idx - (uint)RES];
        if (!is_solid(idx + (uint)RES)) div += vel_y[idx + (uint)RES] - vel_y[idx];
    } else if (gy == 0) {
        if (!is_solid(idx + (uint)RES)) div += vel_y[idx + (uint)RES] - vel_y[idx];
    } else {
        if (!is_solid(idx - (uint)RES)) div += vel_y[idx] - vel_y[idx - (uint)RES];
    }

    // ∂w/∂z
    if (RESZ > 1) {
        if (gz > 0 && gz < (uint)RESZ - 1) {
            if (!is_solid(idx - RNN)) div += vel_z[idx] - vel_z[idx - RNN];
            if (!is_solid(idx + RNN)) div += vel_z[idx + RNN] - vel_z[idx];
        } else if (gz == 0) {
            if (!is_solid(idx + RNN)) div += vel_z[idx + RNN] - vel_z[idx];
        } else {
            if (!is_solid(idx - RNN)) div += vel_z[idx] - vel_z[idx - RNN];
        }
    }

    div_out[idx] = -div;
}
