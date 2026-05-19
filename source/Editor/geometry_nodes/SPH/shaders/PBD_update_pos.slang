
#include "utils.slangh"

ConstantBuffer<PBDConstants> pbd_constants : register(b0);
StructuredBuffer<uint2> ContactPairs : register(t0);

RWBuffer<float3> positions : register(u0);
RWBuffer<float3> velocities : register(u1);
RWBuffer<float3> positions_old : register(u2);

// to zero
RWBuffer<uint> rho : register(u3);


RWStructuredBuffer<int> velocities_sum : register(u4);


//particle_count
[numthreads(32, 1, 1)]
void update_vel(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numParticles)
        return;
    float h = pbd_constants.smoothingRadius;

    //float3 gravity = float3(0, pbd_constants.gravity, 0);

    float dt = pbd_constants.dt;
    float3 dx = positions[i_globalIdx.x] - positions_old[i_globalIdx.x];
    float v_norm = length(dx);
    positions[i_globalIdx.x] = positions[i_globalIdx.x] - int(v_norm > h*dt*50) * (1 - h*dt*50 / v_norm) * dx;

    velocities[i_globalIdx.x] = (positions[i_globalIdx.x] - positions_old[i_globalIdx.x]) / dt ;

    //positions[i_globalIdx.x] = max(positions[i_globalIdx.x], float3(0, 0, 0));
    //positions[i_globalIdx.x] = min(positions[i_globalIdx.x], boundary);
    float3 boundary = float3(2,2,2);
    //velocities[i_globalIdx.x] = (positions[i_globalIdx.x] - positions_old[i_globalIdx.x]) / dt*0.98;
    int3 minflag = int3(positions[i_globalIdx.x] < float3(0, 0, 0));
    int3 maxflag = int3(positions[i_globalIdx.x] > boundary);
    positions[i_globalIdx.x] = -(2 - sqrt(0.7)) * positions[i_globalIdx.x] * minflag + positions[i_globalIdx.x];
    positions[i_globalIdx.x] = -(2-  sqrt(0.7)) * (positions[i_globalIdx.x] - boundary) * maxflag + positions[i_globalIdx.x];
    velocities[i_globalIdx.x] = (1 - (2 -  sqrt(0.7)) * (minflag + maxflag)) * velocities[i_globalIdx.x];

}


[numthreads(32, 1, 1)]
void update_vel_xsph(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numPairs)
        return;

    float h = pbd_constants.smoothingRadius;
    float rho0 = pbd_constants.restDensity;

    uint u1 = ContactPairs[i_globalIdx.x].x;
    uint u2 = ContactPairs[i_globalIdx.x].y;
    float3 r = positions[u1] - positions[u2];
    float3 dv = velocities[u1] - velocities[u2];
    int3 d_sum = int3(dv * ploy6_kernel(r, h) * INT_SCALE);

    InterlockedAdd(velocities_sum[3 * u1 + 0], d_sum.x);
    InterlockedAdd(velocities_sum[3 * u1 + 1], d_sum.y);
    InterlockedAdd(velocities_sum[3 * u1 + 2], d_sum.z);
    InterlockedAdd(velocities_sum[3 * u2 + 0], -d_sum.x);
    InterlockedAdd(velocities_sum[3 * u2 + 1], -d_sum.y);
    InterlockedAdd(velocities_sum[3 * u2 + 2], -d_sum.z);
}


[numthreads(32, 1, 1)]
void update_pos(int2 i_globalIdx : SV_DispatchThreadID)
{
    if (i_globalIdx.x >= pbd_constants.numParticles)
        return;

    float3 gravity = float3(0, pbd_constants.gravity, 0);

    float dt = pbd_constants.dt;
    float constVisc = pbd_constants.constVisc;

    float3 v_sum = float3(velocities_sum[i_globalIdx.x * 3 + 0], velocities_sum[i_globalIdx.x * 3 + 1], velocities_sum[i_globalIdx.x * 3 + 2]) / INT_SCALE;

    velocities[i_globalIdx.x] -= constVisc * v_sum;
    positions_old[i_globalIdx.x] = positions[i_globalIdx.x];
    positions[i_globalIdx.x] += velocities[i_globalIdx.x] * dt + gravity * dt * dt;

    velocities_sum[3 * i_globalIdx.x + 0] = 0;
    velocities_sum[3 * i_globalIdx.x + 1] = 0;
    velocities_sum[3 * i_globalIdx.x + 2] = 0;
    rho[i_globalIdx.x] = 0;

}