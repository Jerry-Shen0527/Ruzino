#include "utils.slangh"

ConstantBuffer<SPHConstants> sph_constants : register(b0);

RWByteAddressBuffer positions : register(u0);
RWByteAddressBuffer velocities : register(u1);

RWByteAddressBuffer pressure : register(u2);
RWByteAddressBuffer viscosity : register(u3);
RWByteAddressBuffer rho : register(u4);

// particle_count
[numthreads(32, 1, 1)]
void main(int2 i_globalIdx: SV_DispatchThreadID)
{
    if (i_globalIdx.x >= sph_constants.numParticles)
        return;

    float3 gravity = float3(0, 0, sph_constants.gravity);

    float dt = sph_constants.dt;
    int i = i_globalIdx.x;
    int3 iF_pres = int3(
        pressure.Load(i * 12),
        pressure.Load(i * 12 + 4),
        pressure.Load(i * 12 + 8));
    int3 iF_visc = int3(
        viscosity.Load(i * 12),
        viscosity.Load(i * 12 + 4),
        viscosity.Load(i * 12 + 8));
    float3 F_pres = float3(iF_pres) / INT_SCALE;
    float3 F_visc = float3(iF_visc) / INT_SCALE;
    float3 pos = asfloat(positions.Load3(i * 12));
    float3 vel = asfloat(velocities.Load3(i * 12));
    float3 pos_o = pos;

    // Get particle mass for acceleration calculation
    float mass = sph_constants.mV;
    float rho_i = float(rho.Load(i * 4)) / INT_SCALE;

    // Update velocity and position
    // Apply forces: a = F/m, v += a*dt
    float3 acceleration = (F_pres + F_visc) / mass + gravity;
    vel += dt * acceleration;
    pos += dt * vel;

    // Simple boundary collision with axis-aligned box [-1,1]^3
    float3 min_boundary = float3(-1, -1, -1);
    float3 max_boundary = float3(1, 1, 1);

    // Check collision with boundaries
    int3 minflag = int3(pos < min_boundary);
    int3 maxflag = int3(pos > max_boundary);

    // Reflect position relative to boundary planes
    pos = pos - 2 * minflag * (pos - min_boundary);
    pos = pos - 2 * maxflag * (pos - max_boundary);

    // Apply damping to velocity on collision
    vel = (1 - 2 * (minflag + maxflag) * sqrt(0.7)) * vel;

    // Recalculate velocity from actual position change (important for stability!)
    vel = (pos - pos_o) / dt;

    // Store updated values
    positions.Store3(i * 12, asuint(pos));
    velocities.Store3(i * 12, asuint(vel));

    // Clear force accumulators for next iteration
    rho.Store(i * 4, 0);
    pressure.Store(i * 12 + 0, 0);
    pressure.Store(i * 12 + 4, 0);
    pressure.Store(i * 12 + 8, 0);
    viscosity.Store(i * 12 + 0, 0);
    viscosity.Store(i * 12 + 4, 0);
    viscosity.Store(i * 12 + 8, 0);
}
