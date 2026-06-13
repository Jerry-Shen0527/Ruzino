// Particle update — Wetbrush Section 4.3, Eq.8-10  (3D)
// Two-step update in non-inertial brush frame (Eq.9) + canvas frame,
// blended by adhesion distance (Eq.10).
// Also applies friction (Eq.8) near solid surfaces.
//
// Z convention: canvas/paper lies at z = grid_center_z - height_extent/2 (the
// bottom of the paint volume). Brush rotates about the canvas normal (z-axis),
// so the angular velocity vector is ω = (0, 0, ω_z).

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_vel : register(t1);
StructuredBuffer<float4> ptcl_color : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
RWStructuredBuffer<float4> ptcl_pos_out : register(u0);
RWStructuredBuffer<float4> ptcl_vel_out : register(u1);
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

    float3 pos = ptcl_pos[idx].xyz;
    float3 vel = ptcl_vel[idx].xyz;
    float3 brush_center = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z);
    float dt = cb.dt;

    // --- Distance to brush center (3D) ---
    float3 r_from_brush = pos - brush_center;
    float d_B = length(r_from_brush);

    // --- Friction force (Eq.8): f = -max((1 - d/D0)^2, 0) * v ---
    float friction_t = max(1.0 - d_B / cb.friction_delta, 0.0);
    float3 friction_force = -friction_t * friction_t * vel;

    // --- Acceleration in canvas frame (gravity kept small; paint is viscous) ---
    float3 a_canvas = friction_force;

    // ======================================================================
    // Step 1: Update in local (brush) frame — Eq.9
    // Full non-inertial frame: rectilinear + centrifugal + Euler + Coriolis
    // ======================================================================
    float beta_L = 0.1;  // Table 1: β_L ∈ [0, 0.2]

    // Brush angular velocity about canvas normal (z-axis)
    float3 omega = float3(0.0, 0.0, cb.brush_angular_vel_z);

    // Position in brush frame: p_k^L = R_L · (p_k - brush_center)
    // For small per-step rotations, approximate R_L ≈ I.
    float3 p_local = r_from_brush;
    float3 v_local = vel;  // friction already slows canvas-frame motion

    // --- All four inertial accelerations from Eq.9 (3D, via cross products) ---
    // 1. Rectilinear: linear accel of brush-frame origin.
    //    Approximated as zero here (handled indirectly by adhesion blend below).
    // 2. Centrifugal: ω × (ω × p_local)
    float3 centrifugal = cross(omega, cross(omega, p_local));
    // 3. Euler acceleration: ω̇ × p_local  (ω̇ estimated from ω_z)
    float omega_dot_z = cb.brush_angular_vel_z / max(dt, 1e-6f) * 0.3;
    float3 euler_accel = cross(float3(0.0, 0.0, omega_dot_z), p_local);
    // 4. Coriolis: 2 ω × v_local
    float3 coriolis = 2.0 * cross(omega, v_local);

    // Total acceleration in local frame (Eq.9):
    //   v̇_k^L = R_L * a_k - β_L * (rect + centrifugal + euler + coriolis)
    float3 accel_local = a_canvas
        - beta_L * (centrifugal + euler_accel + coriolis);

    // Integrate in local frame
    float3 v_local_new = v_local + accel_local * dt;
    float3 p_local_new = p_local + v_local_new * dt;

    // Transform back to canvas frame
    float3 p_prime = p_local_new + brush_center;

    // ======================================================================
    // Step 2: Update in canvas frame — standard Euler
    // ======================================================================
    float3 p_double_prime = pos + vel * dt + 0.5 * a_canvas * dt * dt;

    // ======================================================================
    // Step 3: Blend by adhesion distance (Eq.10)
    // p_new = p'' + max(1 - d_B/D1, 0) * (p' - p'')
    // ======================================================================
    float adhesion = max(1.0 - d_B / cb.D1, 0.0);
    float3 new_pos = p_double_prime + adhesion * (p_prime - p_double_prime);

    // Keep particles inside the paint volume in Z
    float z_lo = cb.grid_center_z - cb.height_extent * 0.5;
    float z_hi = cb.grid_center_z + cb.height_extent * 0.5;
    new_pos.z = clamp(new_pos.z, z_lo, z_hi);

    // Correct velocity from position change
    float3 new_vel = (new_pos - pos) / max(dt, 1e-6f);

    // Kill slow particles far from brush
    float speed = length(new_vel);
    if (d_B > cb.D0 * 1.5 && speed < 0.001) {
        ptcl_alive_out[idx] = 0;
    } else {
        ptcl_alive_out[idx] = 1;
    }

    ptcl_pos_out[idx] = float4(new_pos, 0.0);
    ptcl_vel_out[idx] = float4(new_vel, 0.0);
}
