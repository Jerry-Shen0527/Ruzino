// Damp velocity and increase dryness — 3D version
// Wetbrush Section 4.2: "we also increase the dryness of every grid cell
// by a small amount. Once the dryness reaches a threshold, we ignore its velocity."

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

// oil_density was declared here as an SRV but never sampled — removed (dead
// binding). If §6 oil-density viscosity coupling is ever implemented, it
// belongs in the fluid solve (jacobi), not here.

RWStructuredBuffer<float> vel_x   : register(u0);
RWStructuredBuffer<float> vel_y   : register(u1);
RWStructuredBuffer<float> vel_z   : register(u2);
RWStructuredBuffer<float> wetness : register(u3);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    // Dispatched over the FULL grid (not just the active window), so drying
    // affects paint left behind by a moving brush. Index directly into the
    // global buffer; no window mapping needed.
    int N = cb.res;
    int D = cb.res_z;
    uint total = (uint)N * (uint)N * (uint)D;
    if (dtid.x >= total) return;
    uint idx = dtid.x;

    // Per-dispatch velocity damping (paper §4.2's damp step; the host sends
    // pow(frame_damp, 1/substeps) so decay is substep-count independent).
    // Injected brush velocity would otherwise persist until the cell dries
    // and keep semi-Lagrangian-smearing the paint — the stroke visibly
    // inflates while the active window still covers it.
    float damp = cb.velocity_damp > 0.0 ? cb.velocity_damp : 1.0;
    vel_x[idx] *= damp;
    vel_y[idx] *= damp;
    vel_z[idx] *= damp;

    // Zero velocity where paint is dry
    if (wetness[idx] < 0.01f) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
    }

    // Increase dryness (decrease wetness)
    wetness[idx] = max(0.0f, wetness[idx] - cb.drying_rate * cb.dt);
}
