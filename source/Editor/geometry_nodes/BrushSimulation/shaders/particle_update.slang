// Particle update — Wetbrush Section 4.3, Eq.8-10
// Two-step update with friction near bristles

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float2> ptcl_pos : register(t0);
StructuredBuffer<float2> ptcl_vel : register(t1);
StructuredBuffer<float4> ptcl_color : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
RWStructuredBuffer<float2> ptcl_pos_out : register(u0);
RWStructuredBuffer<float2> ptcl_vel_out : register(u1);
RWStructuredBuffer<uint> ptcl_alive_out : register(u2);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) {
        ptcl_alive_out[idx] = 0;
        return;
    }

    float2 pos = ptcl_pos[idx];
    float2 vel = ptcl_vel[idx];
    float2 brush_pos2 = float2(cb.brush_pos_x, cb.brush_pos_y);
    float d = length(pos - brush_pos2);

    // Friction force (Eq.8): f = -max((1 - d/D0)^2, 0) * v
    float friction = max(1.0 - d / cb.D0, 0.0);
    friction = friction * friction;
    float2 friction_force = -friction * vel;

    // Two-step update (Eq.10)
    float dt = cb.dt;

    // Local frame: transform to brush-local, advance, transform back
    float2 r = pos - brush_pos2;
    // Simplified: assume brush moves linearly
    float2 p_local = pos - brush_pos2 * dt;
    float2 p_prime = p_local + vel * dt + brush_pos2;

    // Canvas frame: standard advect
    float2 p_double_prime = pos + vel * dt;

    // Blend based on distance to brush
    float blend = max(1.0 - d / cb.brush_radius, 0.0);
    float2 new_pos = p_double_prime + blend * (p_prime - p_double_prime);

    // Apply friction to velocity
    float2 new_vel = vel + friction_force * dt;

    // Kill slow particles far from brush
    float speed = length(new_vel);
    if (d > cb.D0 * 1.5 && speed < 0.001) {
        ptcl_alive_out[idx] = 0;
    } else {
        ptcl_alive_out[idx] = 1;
    }

    ptcl_pos_out[idx] = new_pos;
    ptcl_vel_out[idx] = new_vel;
}

