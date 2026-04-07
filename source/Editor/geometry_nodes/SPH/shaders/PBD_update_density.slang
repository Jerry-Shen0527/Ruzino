#include "utils.slangh"


ConstantBuffer<PBDConstants> pbd_constants : register(b0);
StructuredBuffer<uint2> ContactPairs : register(t0);
RWBuffer<uint> rho : register(u0);
RWBuffer<float3> positions : register(u1);


//pair_count
[numthreads(32, 1, 1)]
void update_PBD_density(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numPairs)
        return;
    float mV = pbd_constants.mV;
    float rho0 = pbd_constants.restDensity;
    float h = pbd_constants.smoothingRadius;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float3 r = positions[u1] - positions[u2];

    float Drho = mV * ploy6_kernel(r, h);
    uint iDrho = uint(Drho * INT_SCALE);
    InterlockedAdd(rho[u1], iDrho);
    InterlockedAdd(rho[u2], iDrho);

}

//particle_count
[numthreads(32, 1, 1)]
void update_PBD_density_once(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numParticles)
        return;
    float mV = pbd_constants.mV;
    float h = pbd_constants.smoothingRadius;
    float rho0 = pbd_constants.restDensity;
    float Drho = mV * ploy6_kernel(float3(0, 0, 0), h);
    uint iDrho = uint(Drho * INT_SCALE);
    //InterlockedAdd(rho[i_globalIdx.x], iDrho);
    rho[i_globalIdx.x] += iDrho;

}
