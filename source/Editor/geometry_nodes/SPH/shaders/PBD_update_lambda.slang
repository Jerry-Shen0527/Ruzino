#include "utils.slangh"


ConstantBuffer<PBDConstants> pbd_constants : register(b0);
StructuredBuffer<uint2> ContactPairs : register(t0);

RWBuffer<uint> rho : register(u0);
RWBuffer<float> lambda : register(u1);
RWStructuredBuffer<int> dpConstrain : register(u2);
RWStructuredBuffer<int> dpConstrain2 : register(u3);
RWBuffer<float3> positions : register(u4);

//pair_count
//sum_j D_{pk}W(pi-pj,h)  k=i
[numthreads(32, 1, 1)]
void update_DpC(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numPairs)
        return;
    float mV = pbd_constants.mV;
    float h = pbd_constants.smoothingRadius;
    float rho0 = pbd_constants.restDensity;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float3 r = positions[u1] - positions[u2];
    float3 dpC = spiky_kernel_derivative(r, h) / rho0;
    int dpC2 = int(dot(dpC, dpC)/0xffff);

    int3 dpCi = int3(dpC / 0xf);
    int3 dpCj = int3(dpC / 0xf);
    //if (length(r)>0)
    //    lambda[0] = dot(dpC, dpC);
    //if (dot(dpC, dpC) > 0)
    //    lambda[0] = int(dot(dpC, dpC) / 0xff);

    InterlockedAdd(dpConstrain[3 * u1 + 0], dpCi.x);
    InterlockedAdd(dpConstrain[3 * u1 + 1], dpCi.y);
    InterlockedAdd(dpConstrain[3 * u1 + 2], dpCi.z);
    InterlockedAdd(dpConstrain[3 * u2 + 0], dpCj.x);
    InterlockedAdd(dpConstrain[3 * u2 + 1], dpCj.y);
    InterlockedAdd(dpConstrain[3 * u2 + 2], dpCj.z);

    InterlockedAdd(dpConstrain2[u1], dpC2);
    InterlockedAdd(dpConstrain2[u2], dpC2);


}


// particle_count
// lambda denominator - sum
// sum |D_k C_i|^2
[numthreads(32, 1, 1)]
void update_lambda(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numParticles)
        return;
    float mV = pbd_constants.mV;
    float h = pbd_constants.smoothingRadius;
    float rho_0 = pbd_constants.restDensity;

    uint u1 = i_globalIdx.x;
    float dpC2 = float(dpConstrain2[u1])*0xffff ;
    float3 dpCi = float3(dpConstrain[3 * u1 + 0], dpConstrain[3 * u1 + 1], dpConstrain[3 * u1 + 2])*0xf;

    lambda[u1] = -max((float(rho[u1]) / INT_SCALE) - rho_0, 0) / rho_0 / (dpC2 + dot(dpCi, dpCi) + 0.0001);
    //lambda[0] = float(dpConstrain[3 * 1000 + 0]);
    //positions[u1] += float3(0, lambda[u1], 0);
    dpConstrain2[u1] = 0;
    dpConstrain[3 * u1 + 0] = 0;
    dpConstrain[3 * u1 + 1] = 0;
    dpConstrain[3 * u1 + 2] = 0;

}