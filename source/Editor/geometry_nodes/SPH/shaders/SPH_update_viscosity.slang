#include "utils.slangh"

ConstantBuffer<SPHConstants> sph_constants : register(b0);
StructuredBuffer<uint2> ContactPairs : register(t0);
StructuredBuffer<float3> positions : register(t1);
StructuredBuffer<float3> velocities : register(t2);
ByteAddressBuffer rho : register(t3);

RWByteAddressBuffer viscosity : register(u0);

// pair_count
[numthreads(32, 1, 1)]
void main(int2 i_globalIdx: SV_DispatchThreadID)
{
    if (i_globalIdx.x >= sph_constants.numPairs)
        return;
    float mass = sph_constants.mV;  // mV stores particle mass
    float h = sph_constants.smoothingRadius;
    float visc = sph_constants.constVisc;
    float rho_0 = sph_constants.restDensity;
    float surf = sph_constants.constSurf;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float3 r = positions[u1] - positions[u2];
    float r2 = dot(r, r);
    float3 dv = velocities[u1] - velocities[u2];
    float rho_i = max(float(rho.Load(u1 * 4)) / INT_SCALE, rho_0);
    float rho_j = max(float(rho.Load(u2 * 4)) / INT_SCALE, rho_0);

    // Standard SPH viscosity force (Monaghan 1992)
    // F = m * Σ mⱼ * (μᵢ + μⱼ) * (vⱼ - vᵢ)·rᵢⱼ / ((ρᵢ + ρⱼ)/2 * (|rᵢⱼ|² + ε)) * ∇W
    // Simplified: using same viscosity for all particles
    float rho_avg = (rho_i + rho_j) * 0.5;
    float3 grad_W = cubic_kernel_derivative(r, h);

    // Enhanced viscosity formulation with higher coefficient
    float visc_factor = 2.0 * visc * mass * dot(dv, r) / (rho_avg * (r2 + 0.01 * h * h));
    float3 F_visc = visc_factor * grad_W;

    // Surface tension force
    // When particles are too close (distance < diameter), clamp to avoid singularity
    float d = sph_constants.particleDiameter;
    float d2 = d * d;
    float3 f_surf;
    if (r2 > d2)
        f_surf = surf * mass * r * cubic_kernel(r, h);
    else
        // Use diameter as minimum distance (not radius!)
        f_surf = surf * mass * normalize(r) * d * cubic_kernel(float3(d, 0, 0), h);

    int3 iF_visci = int3((F_visc / rho_j - f_surf) * INT_SCALE);
    int3 iF_viscj = int3(-(F_visc / rho_i - f_surf) * INT_SCALE);

    viscosity.InterlockedAdd(u1 * 12 + 0, iF_visci.x);
    viscosity.InterlockedAdd(u1 * 12 + 4, iF_visci.y);
    viscosity.InterlockedAdd(u1 * 12 + 8, iF_visci.z);
    viscosity.InterlockedAdd(u2 * 12 + 0, iF_viscj.x);
    viscosity.InterlockedAdd(u2 * 12 + 4, iF_viscj.y);
    viscosity.InterlockedAdd(u2 * 12 + 8, iF_viscj.z);
}
