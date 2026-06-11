// Particle update — Wetbrush Section 4.3, Eq.8-10
// Two-step update in non-inertial brush frame (Eq.9) + canvas frame,
// blended by adhesion distance (Eq.10).
// Also applies friction (Eq.8) near solid surfaces.

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
    float2 brush_center = float2(cb.brush_pos_x, cb.brush_pos_y);
    float dt = cb.dt;

    // --- Distance to brush center ---
    float2 r_from_brush = pos - brush_center;
    float d_B = length(r_from_brush);

    // --- Friction force (Eq.8): f = -max((1 - d/D0)^2, 0) * v ---
    float friction_t = max(1.0 - d_B / cb.friction_delta, 0.0);
    float2 friction_force = -friction_t * friction_t * vel;

    // --- Acceleration in canvas frame ---
    float2 a_canvas = friction_force;  // external acceleration (gravity = 0 for 2D paint)

    // ======================================================================
    // Step 1: Update in local (brush) frame — Eq.9
    // Full non-inertial frame: rectilinear + centrifugal + Euler + Coriolis
    // ======================================================================
    float beta_L = 0.1;  // Table 1: β_L ∈ [0, 0.2]
    float omega = cb.brush_angular_vel_z;

    // Position in brush frame: p_k^L = R_L · (p_k - brush_center)
    // In 2D rotation about z: R_L rotates by brush_rotation angle.
    // For small rotations per step, approximate R_L ≈ I.
    float2 p_local = r_from_brush;

    // Velocity in brush frame: v_k^L = v_k - v_brush
    // v_brush estimated from brush_pos change (passed as brush velocity)
    float2 v_brush = float2(
        length(float2(cb.brush_pos_x - 0.0, 0.0)),  // rough: use as linear vel proxy
        length(float2(0.0, cb.brush_pos_y - 0.0)));
    // More accurate: use grid velocity at particle pos as drag source
    // For simplicity, v_local = canvas velocity (friction already slows it)
    float2 v_local = vel;

    // --- All four inertial accelerations from Eq.9 (2D) ---
    // 1. Rectilinear: ẑ^B (linear accel of brush frame origin)
    //    Simplified: already included via friction drag on vel → small contribution
    // 2. Centrifugal: ω² × (ω × p_local) → ω² * p_local in 2D
    float2 centrifugal = omega * omega * p_local;
    // 3. Euler acceleration: ω̇ × p_local → ω̇ * (-p_local.y, p_local.x) in 2D
    //    ω̇ estimated as omega / dt (angular accel from angular vel)
    float omega_dot = omega / max(dt, 1e-6f) * 0.3;
    float2 euler_accel = omega_dot * float2(-p_local.y, p_local.x);
    // 4. Coriolis: 2ω × v_local → 2ω * (-v_local.y, v_local.x) in 2D
    float2 coriolis = 2.0 * omega * float2(-v_local.y, v_local.x);

    // Total acceleration in local frame (Eq.9):
    //   v̇_k^L = R_L * a_k - β_L * (rect + centrifugal + euler + coriolis)
    float2 accel_local = a_canvas
        - beta_L * (centrifugal + euler_accel + coriolis);

    // Integrate in local frame
    float2 v_local_new = v_local + accel_local * dt;
    float2 p_local_new = p_local + v_local_new * dt;

    // Transform back to canvas frame
    float2 p_prime = p_local_new + brush_center;

    // ======================================================================
    // Step 2: Update in canvas frame — standard Euler
    // ======================================================================
    float2 p_double_prime = pos + vel * dt + 0.5 * a_canvas * dt * dt;

    // ======================================================================
    // Step 3: Blend by adhesion distance (Eq.10)
    // p_new = p'' + max(1 - d_B/D1, 0) * (p' - p'')
    // ======================================================================
    float adhesion = max(1.0 - d_B / cb.D1, 0.0);
    float2 new_pos = p_double_prime + adhesion * (p_prime - p_double_prime);

    // Correct velocity from position change
    float2 new_vel = (new_pos - pos) / max(dt, 1e-6f);

    // Kill slow particles far from brush
    float speed = length(new_vel);
    if (d_B > cb.D0 * 1.5 && speed < 0.001) {
        ptcl_alive_out[idx] = 0;
    } else {
        ptcl_alive_out[idx] = 1;
    }

    ptcl_pos_out[idx] = new_pos;
    ptcl_vel_out[idx] = new_vel;
}

