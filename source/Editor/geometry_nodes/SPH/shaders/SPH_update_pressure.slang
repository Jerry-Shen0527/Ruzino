// pressure_hlsl
#include "utils.slangh"

ConstantBuffer<SPHConstants> sph_constants : register(b0);

StructuredBuffer<uint2> ContactPairs : register(t0);
StructuredBuffer<float3> positions : register(t1);

RWByteAddressBuffer pressure : register(u0);
ByteAddressBuffer rho : register(t2);

// pair_count
[numthreads(32, 1, 1)]
void main(int2 i_globalIdx: SV_DispatchThreadID)
{
    if (i_globalIdx.x >= sph_constants.numPairs)
        return;
    float mass = sph_constants.mV;  // mV stores particle mass
    float h = sph_constants.smoothingRadius;
    float rho_0 = sph_constants.restDensity;
    float press_k = sph_constants.constPress;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float3 r = positions[u1] - positions[u2];
    float rho_i = max(float(rho.Load(u1 * 4)) / INT_SCALE, rho_0);
    float rho_j = max(float(rho.Load(u2 * 4)) / INT_SCALE, rho_0);

    // Tait equation: p = k * ((ρ/ρ₀)^γ - 1), using γ=2 for simplification
    float p_i = press_k * (pow(max(rho_i / rho_0, 1.0), 2.0) - 1.0);
    float p_j = press_k * (pow(max(rho_j / rho_0, 1.0), 2.0) - 1.0);

    // Pressure force: F = -m * (pᵢ/ρᵢ² + pⱼ/ρⱼ²) * ∇W
    float3 Dp = mass * (p_i / (rho_i * rho_i) + p_j / (rho_j * rho_j)) *
                cubic_kernel_derivative(r, h);
    float3 F_pres_1 = -Dp;
    float3 F_pres_2 = Dp;
    int3 iF_pres_1 = int3(F_pres_1 * INT_SCALE);
    int3 iF_pres_2 = int3(F_pres_2 * INT_SCALE);

    pressure.InterlockedAdd(u1 * 12 + 0, iF_pres_1.x);
    pressure.InterlockedAdd(u1 * 12 + 4, iF_pres_1.y);
    pressure.InterlockedAdd(u1 * 12 + 8, iF_pres_1.z);
    pressure.InterlockedAdd(u2 * 12 + 0, iF_pres_2.x);
    pressure.InterlockedAdd(u2 * 12 + 4, iF_pres_2.y);
    pressure.InterlockedAdd(u2 * 12 + 8, iF_pres_2.z);
}
