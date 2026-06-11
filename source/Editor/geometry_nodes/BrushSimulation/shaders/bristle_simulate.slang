// Bristle simulation — Wetbrush Section 4.1, Eq.2
// One thread per bristle. Each bristle is a chain of M vertices
// driven by spring forces + ALL FOUR non-inertial frame accelerations:
//   1. Rectilinear (Euler):    v_hat^B
//   2. Centrifugal:            omega^B × (omega^B × x_i^B)
//   3. Euler acceleration:     omega_dot^B × x_i^B
//   4. Coriolis:               2 * omega^B × v_i^B
// After dynamics: inextensible + bending constraints (PBD).

#include "common.slangh"

struct BristleV2 { float2 pos, vel; };

ConstantBuffer<BristleConstants> cb : register(b0);
RWStructuredBuffer<BristleV2> bristle_data : register(u0);  // Nb * M

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int bi = int(tid.x);
    if (bi >= cb.num_bristles) return;

    int M = cb.verts_per_bristle;
    int base = bi * M;

    // Root position: evenly distributed on circle
    float angle = 2.0 * 3.14159265 * float(bi) / float(cb.num_bristles);
    float2 root_offset = cb.brush_radius * float2(cos(angle), sin(angle));

    // Rotate offset by brush rotation (R_B transform)
    float ca = cos(cb.brush_rotation), sa = sin(cb.brush_rotation);
    float2 rotated = float2(
        ca * root_offset.x - sa * root_offset.y,
        sa * root_offset.x + ca * root_offset.y);
    float2 brush_pos = float2(cb.brush_pos_x, cb.brush_pos_y);
    float2 root_pos = brush_pos + rotated;

    // Vertex 0: pinned to root
    bristle_data[base].pos = root_pos;
    bristle_data[base].vel = float2(cb.brush_vel_x, cb.brush_vel_y);

    // Non-inertial frame quantities
    float omega = cb.brush_angular_vel;
    float beta_B = cb.beta_B;
    float dt = cb.dt;
    float rest_len = cb.brush_radius * 1.5 / float(M);

    // Estimate angular acceleration ω̇ from angular velocity
    // In 2D: ω̇^B is the change in angular velocity (simplified as current ω / dt)
    float omega_dot = omega / max(dt, 1e-6f) * 0.5; // damped estimate

    // Rectilinear acceleration (acceleration of brush frame origin)
    float2 v_hat_B = float2(cb.brush_vel_x, cb.brush_vel_y);

    // Simulate vertices 1..M-1
    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float2 pos = bristle_data[idx].pos;
        float2 vel = bristle_data[idx].vel;

        // Spring force toward previous vertex
        float2 diff = bristle_data[idx - 1].pos - pos;
        float dist = length(diff);
        float2 spring_f = cb.spring_k * (dist - rest_len) * normalize(diff + 1e-8);

        // Position in brush frame: x_i^B = pos - brush_pos
        float2 x_B = pos - brush_pos;

        // Velocity in brush frame: v_i^B = vel - v_brush
        float2 v_B = vel - v_hat_B;

        // === All four non-inertial frame accelerations (Eq.2) ===
        // 1. Rectilinear (Euler): -v_hat_B (linear acceleration of frame)
        //    (simplified: brush velocity acts as drag)
        float2 euler_rect = v_hat_B;

        // 2. Centrifugal: ω² × r  →  ω² * r (in 2D, points radially outward)
        float2 centrifugal = omega * omega * x_B;

        // 3. Euler angular acceleration: ω̇ × x_i^B
        //    In 2D: ω̇ × r = ω̇ * (-r.y, r.x)
        float2 euler_angular = omega_dot * float2(-x_B.y, x_B.x);

        // 4. Coriolis: -2ω × v_i^B
        //    In 2D: -2ω * (-v.y, v.x)
        float2 coriolis = -2.0 * omega * float2(-v_B.y, v_B.x);

        // Total acceleration (Eq.2):
        //   ȧ_i^B = R_B * a_i - beta_B * (v_hat + centrifugal + euler_angular + coriolis)
        //   a_i = spring + damping (canvas frame external forces)
        float2 external_a = spring_f - cb.damping * vel;
        float2 inertial_sum = euler_rect + centrifugal + euler_angular + coriolis;
        float2 accel = external_a - beta_B * inertial_sum;

        vel += accel * dt;
        pos += vel * dt;

        bristle_data[idx].pos = pos;
        bristle_data[idx].vel = vel;
    }

    // === Constraint enforcement (Position-Based Dynamics) ===
    for (int iter = 0; iter < 3; iter++) {
        // Inextensible constraint — enforce max segment length
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float2 diff = bristle_data[idx].pos - bristle_data[idx - 1].pos;
            float d = length(diff);
            if (d > rest_len * 1.05) {
                float2 correction = (d - rest_len) * normalize(diff + 1e-8) * 0.5;
                bristle_data[idx].pos -= correction;
                if (vi > 1)
                    bristle_data[idx - 1].pos += correction;
            }
        }

        // Bending constraint — keep angle between consecutive segments close to straight
        // Uses the PBD bending formulation: project tip vertex toward
        // the midpoint of the line between (vi-1) and (vi+1)
        float bend_stiffness = 0.3; // [0, 1]
        for (int vi = 2; vi < M - 1; vi++) {
            int idx = base + vi;
            float2 p0 = bristle_data[idx - 1].pos;
            float2 p1 = bristle_data[idx].pos;
            float2 p2 = bristle_data[idx + 1].pos;

            // Midpoint between neighbors
            float2 mid = (p0 + p2) * 0.5;
            float2 correction = (mid - p1) * bend_stiffness;
            bristle_data[idx].pos += correction;
        }
    }
}
