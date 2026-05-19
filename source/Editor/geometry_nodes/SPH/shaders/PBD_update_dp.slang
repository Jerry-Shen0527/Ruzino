#include "utils.slangh"

ConstantBuffer<PBDConstants> pbd_constants : register(b0);
StructuredBuffer<uint2> ContactPairs : register(t0);
Buffer<float> lambda : register(t1);

RWBuffer<float3> positions : register(u0);
RWStructuredBuffer<int> dposition : register(u1);
RWBuffer<uint> rho : register(u2);


//pair_count
[numthreads(32, 1, 1)]
void update_dp(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numPairs)
        return;
    float mV = pbd_constants.mV;
    float h = pbd_constants.smoothingRadius;
    float rho_0 = pbd_constants.restDensity;
    float constSurf = pbd_constants.constSurf;
    float mass = mV * rho_0;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float lmd1 = lambda[u1];
    float lmd2 = lambda[u2];
    float3 r = positions[u1] - positions[u2];

    float scorr = -constSurf * pow(ploy6_kernel(r, h) / ploy6_kernel(float3(0, 0.2 * h, 0), h), 4);

    float3 dpi = (lmd1 + lmd2 + scorr) * spiky_kernel_derivative(r, h) / rho_0;

    int3 Dpi = int3(dpi * INT_SCALE );
    int3 Dpj = int3(-dpi * INT_SCALE );

    InterlockedAdd(dposition[3 * u1 + 0], Dpi.x);
    InterlockedAdd(dposition[3 * u1 + 1], Dpi.y);
    InterlockedAdd(dposition[3 * u1 + 2], Dpi.z);
    InterlockedAdd(dposition[3 * u2 + 0], Dpj.x);
    InterlockedAdd(dposition[3 * u2 + 1], Dpj.y);
    InterlockedAdd(dposition[3 * u2 + 2], Dpj.z);
}


//particle_count
[numthreads(32, 1, 1)]
void update_p(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numParticles)
        return;
    float mV = pbd_constants.mV;
    float h = pbd_constants.smoothingRadius;
    float rho0 = pbd_constants.restDensity;

    //positions[0].x = length(Dpi);
    uint u1 = i_globalIdx.x;
    float3 Dp = float3(dposition[3 * u1 + 0], dposition[3 * u1 + 1], dposition[3 * u1 + 2]) / INT_SCALE ;
    //if (i_globalIdx.x==500)
    positions[u1] += Dp;
    //positions[0].x = float(rho[500])/INT_SCALE;
    dposition[3 * u1 + 0] = 0;
    dposition[3 * u1 + 1] = 0;
    dposition[3 * u1 + 2] = 0;
    rho[u1] = 0;

}