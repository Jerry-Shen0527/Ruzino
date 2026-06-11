// Damp velocity and increase dryness — 3D version
// Wetbrush Section 4.2: "we also increase the dryness of every grid cell
// by a small amount. Once the dryness reaches a threshold, we ignore its velocity."

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

RWStructuredBuffer<float> vel_x   : register(u0);
RWStructuredBuffer<float> vel_y   : register(u1);
RWStructuredBuffer<float> vel_z   : register(u2);
RWStructuredBuffer<float> wetness : register(u3);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    int total = cb.res * cb.res * cb.res_z;
    if (idx >= (uint)total) return;

    // Damp velocity (viscous drag)
    vel_x[idx] *= 0.98f;
    vel_y[idx] *= 0.98f;
    vel_z[idx] *= 0.98f;

    // Zero velocity where paint is dry
    if (wetness[idx] < 0.01f) {
        vel_x[idx] = 0.0f;
        vel_y[idx] = 0.0f;
        vel_z[idx] = 0.0f;
    }

    // Increase dryness (decrease wetness)
    wetness[idx] = max(0.0f, wetness[idx] - cb.drying_rate * cb.dt);
}
