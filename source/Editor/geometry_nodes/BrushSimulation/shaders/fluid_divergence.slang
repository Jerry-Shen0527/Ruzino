// Compute velocity divergence for pressure projection — 3D version
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// 3D: div = ∂u/∂x + ∂v/∂y + ∂w/∂z
// Uses central differences with Neumann BC at boundaries.

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
    int N = cb.res;
    int D = cb.res_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint x = c.x, y = c.y, z = c.z, idx = c.idx;
    uint NN = (uint)(N * N);

    if (is_solid(idx)) {
        // Solid cell: no divergence contribution (acts as wall).
        div_out[idx] = 0.0f;
        return;
    }

    float div = 0.0f;

    // ∂u/∂x — skip solid neighbors (no-flux across solid interfaces)
    if (x > 0 && x < (uint)N - 1) {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else if (x == 0) {
        if (!is_solid(idx + 1)) div += vel_x[idx + 1] - vel_x[idx];
    } else {
        if (!is_solid(idx - 1)) div += vel_x[idx] - vel_x[idx - 1];
    }

    // ∂v/∂y
    if (y > 0 && y < (uint)N - 1) {
        if (!is_solid(idx - (uint)N)) div += vel_y[idx] - vel_y[idx - (uint)N];
        if (!is_solid(idx + (uint)N)) div += vel_y[idx + (uint)N] - vel_y[idx];
    } else if (y == 0) {
        if (!is_solid(idx + (uint)N)) div += vel_y[idx + (uint)N] - vel_y[idx];
    } else {
        if (!is_solid(idx - (uint)N)) div += vel_y[idx] - vel_y[idx - (uint)N];
    }

    // ∂w/∂z
    if (D > 1) {
        if (z > 0 && z < (uint)D - 1) {
            if (!is_solid(idx - NN)) div += vel_z[idx] - vel_z[idx - NN];
            if (!is_solid(idx + NN)) div += vel_z[idx + NN] - vel_z[idx];
        } else if (z == 0) {
            if (!is_solid(idx + NN)) div += vel_z[idx + NN] - vel_z[idx];
        } else {
            if (!is_solid(idx - NN)) div += vel_z[idx] - vel_z[idx - NN];
        }
    }

    div_out[idx] = -div;
}
