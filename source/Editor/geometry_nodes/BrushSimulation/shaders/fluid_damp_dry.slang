// Damp velocity and increase dryness — 3D version
// Wetbrush Section 4.2: "we also increase the dryness of every grid cell
// by a small amount. Once the dryness reaches a threshold, we ignore its velocity."
// §6: oil density controls viscosity — higher oil density → less viscous drag
// (thinner, flatter paint); lower oil density → more drag (thicker paint).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> oil_density : register(t0);  // §6 per-cell oil density

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

    // Oil-density-modulated viscous drag (§6). Low oil density → thick →
    // strong drag (factor → 0.9); high oil density → thin → weak drag
    // (factor → 0.99). Linear blend across [0, 1].
    float oil = clamp(oil_density[idx], 0.0, 1.0);
    float damp = lerp(0.90f, 0.99f, oil);
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
