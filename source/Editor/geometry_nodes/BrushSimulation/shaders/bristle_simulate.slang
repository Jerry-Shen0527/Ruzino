// Bristle simulation — Wetbrush Section 4.1, Eq.2 (3D)
// One thread per bristle. Each bristle is a chain of M vertices driven by
// spring forces + ALL FOUR non-inertial frame accelerations in full 3D:
//   1. Rectilinear (Euler):    v_hat^B  (linear accel of brush-frame origin)
//   2. Centrifugal:            omega^B × (omega^B × x_i^B)
//   3. Euler acceleration:     omega_dot^B × x_i^B
//   4. Coriolis:               2 * omega^B × v_i^B
// After dynamics: inextensible + bending constraints (PBD).
//
// Storage: BristleV3 vertices packed as float4 (pos.xyz + pad, vel.xyz + pad)
// in a RWStructuredBuffer<float4> to avoid the float3 stride hazard.

#include "common.slangh"

// Each bristle vertex occupies 2 consecutive float4 slots: [pos, vel].
#define BRV_STRIDE 2

ConstantBuffer<BristleConstants> cb : register(b0);
RWStructuredBuffer<float4> bristle_data : register(u0);  // Nb * M * 2

float3 load_pos(int idx) { return bristle_data[idx * BRV_STRIDE].xyz; }
float3 load_vel(int idx) { return bristle_data[idx * BRV_STRIDE + 1].xyz; }
void store_pos(int idx, float3 p) { bristle_data[idx * BRV_STRIDE] = float4(p, 0.0); }
void store_vel(int idx, float3 v) { bristle_data[idx * BRV_STRIDE + 1] = float4(v, 0.0); }

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int bi = int(tid.x);
    if (bi >= cb.num_bristles) return;

    int M = cb.verts_per_bristle;
    int base = bi * M;

    // Root position: distribute bristles across the 2D brush footprint (a
    // filled disk) using a golden-angle spiral (Vogel/sunflower pattern) —
    // gives a near-uniform area filling rather than a hollow ring.
    float golden_angle = 2.39996323;  // radians (~137.5°)
    float angle = golden_angle * float(bi);
    float r_frac = sqrt((float(bi) + 0.5) / float(cb.num_bristles));
    float3 root_offset = cb.brush_radius * r_frac * float3(cos(angle), sin(angle), 0.0);

    float3 brush_pos = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z);
    float3 brush_vel = float3(cb.brush_vel_x, cb.brush_vel_y, cb.brush_vel_z);

    // Vertex 0: pinned to root
    float3 root_pos = brush_pos + root_offset;
    store_pos(base, root_pos);
    store_vel(base, brush_vel);

    // Bristle rest length per segment (chain points from root toward canvas).
    float beta_B = cb.beta_B;
    float dt = cb.dt;
    float rest_len = cb.brush_radius * 1.5 / float(M);
    // Vertical extent of the bristle (root → tip toward the canvas, -Z).
    float3 down_dir = float3(0.0, 0.0, -1.0);

    // Non-inertial frame quantities (full 3D). omega is rad/s; omega_dot is
    // rad/s², both finite-differenced on the host from the previous frame.
    float3 omega = float3(cb.brush_angular_vel_x, cb.brush_angular_vel_y, cb.brush_angular_vel_z);
    float3 omega_dot = float3(cb.brush_angular_accel_x, cb.brush_angular_accel_y, cb.brush_angular_accel_z);
    // Frame-origin linear acceleration a_B (Eq.2 rectilinear term).
    float3 a_B = float3(cb.brush_accel_x, cb.brush_accel_y, cb.brush_accel_z);

    // Simulate vertices 1..M-1
    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float3 pos = load_pos(idx);
        float3 vel = load_vel(idx);

        // First-frame initialization: lay the bristle out as a straight
        // vertical chain from the root toward the canvas (down -Z). Detected
        // by an all-zero persisted position (buffer is zero-initialized).
        if (pos.x == 0.0 && pos.y == 0.0 && pos.z == 0.0) {
            pos = root_pos + down_dir * rest_len * float(vi);
        }

        // Structural spring toward previous vertex (i ↔ i−1)
        float3 diff = load_pos(idx - 1) - pos;
        float dist = length(diff);
        float3 structural_f = cb.spring_k * (dist - rest_len) * normalize(diff + 1e-8);

        // Shear spring toward the previous vertex of the previous vertex
        // (i ↔ i−2): resists shearing between adjacent segments.
        float3 shear_f = float3(0,0,0);
        if (vi >= 2) {
            float3 d2 = load_pos(idx - 2) - pos;
            float dist2 = length(d2);
            float rest2 = rest_len * 2.0;
            shear_f = cb.spring_k * 0.5 * (dist2 - rest2) * normalize(d2 + 1e-8);
        }

        // === Non-inertial brush frame integration (Wetbrush Eq.2) ===
        // Convert into the brush frame, integrate there, convert back. With
        // β_B → 0 the vertex is carried rigidly by the brush (position-based
        // adhesion); β_B = 1 recovers canvas-frame Newton. R_B = I here
        // (brush frame differs from canvas frame by translation only, since
        // brush_rotation only rotates the footprint, not the integration
        // frame), so R_B·a_i = a_i.
        float3 x_B = pos - brush_pos;     // brush-frame position
        float3 v_B = vel - brush_vel;     // brush-frame velocity

        // External acceleration a_i in the CANVAS frame (springs + damping).
        // Damping acts on the canvas-frame velocity so that bristles moving
        // with the brush (v ≈ brush_vel) are not over-damped.
        float3 external_a = structural_f + shear_f - cb.damping * vel;

        // === All four non-inertial frame accelerations (Eq.2, 3D) ===
        // 1. Rectilinear: linear acceleration of the frame origin (a_B).
        float3 euler_rect = a_B;

        // 2. Centrifugal: omega × (omega × x_B)
        float3 centrifugal = cross(omega, cross(omega, x_B));

        // 3. Euler angular acceleration: omega_dot × x_B
        float3 euler_angular = cross(omega_dot, x_B);

        // 4. Coriolis: -2 * omega × v_B
        float3 coriolis = -2.0 * cross(omega, v_B);

        // Eq.2: dv_i^B/dt = R_B·a_i − β_B·(rect + centrifugal + euler + coriolis)
        float3 inertial_sum = euler_rect + centrifugal + euler_angular + coriolis;
        float3 accel_B = external_a - beta_B * inertial_sum;

        // Integrate IN THE BRUSH FRAME.
        v_B += accel_B * dt;
        x_B += v_B * dt;

        // Convert back to canvas frame.
        vel = v_B + brush_vel;
        pos = x_B + brush_pos;

        store_pos(idx, pos);
        store_vel(idx, vel);
    }

    // === Constraint enforcement (Position-Based Dynamics) ===
    // Paper §4.1: "enforce inextensible and bending constraints on vertex
    // positions AND velocities". We accumulate the per-vertex position
    // correction Δp across all constraint iterations, then fold Δp/dt into
    // the velocity (with stiffness-style damping) at the end — the standard
    // PBD velocity update (Müller et al. 2007, Macklin & Müller 2013).
    float3 delta_pos[64];
    [unroll] for (int zi = 0; zi < 64; zi++) delta_pos[zi] = float3(0,0,0);

    for (int iter = 0; iter < 3; iter++) {
        // Inextensible constraint — enforce max segment length
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float3 diff = load_pos(idx) - load_pos(idx - 1);
            float d = length(diff);
            if (d > rest_len * 1.05) {
                float3 correction = (d - rest_len) * normalize(diff + 1e-8) * 0.5;
                store_pos(idx, load_pos(idx) - correction);
                delta_pos[vi] -= correction;
                if (vi > 1) {
                    store_pos(idx - 1, load_pos(idx - 1) + correction);
                    delta_pos[vi - 1] += correction;
                }
            }
        }

        // Bending constraint — project tip toward midpoint of neighbors
        float bend_stiffness = 0.3; // [0, 1]
        for (int vi = 2; vi < M - 1; vi++) {
            int idx = base + vi;
            float3 p0 = load_pos(idx - 1);
            float3 p1 = load_pos(idx);
            float3 p2 = load_pos(idx + 1);

            float3 mid = (p0 + p2) * 0.5;
            float3 correction = (mid - p1) * bend_stiffness;
            store_pos(idx, p1 + correction);
            delta_pos[vi] += correction;
        }

        // Canvas collision + pressure splaying (§4.1, Fig 3a).
        // Vertices cannot penetrate below the canvas plane. Vertices in
        // contact are pushed radially outward (away from the brush XY
        // center) proportional to pressure — this produces the fan/splay
        // footprint when the brush is pressed down.
        float pressure = max(cb.brush_pressure, 0.0);
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float3 p = load_pos(idx);

            // Non-penetration: clamp to canvas plane.
            if (p.z < cb.canvas_z) {
                float penetration = cb.canvas_z - p.z;
                float3 old_p = p;
                p.z = cb.canvas_z;

                // Splay: push the contact point radially outward in XY by
                // an amount proportional to penetration depth and pressure.
                // The deeper the press / higher the pressure, the wider the
                // footprint spreads. Tip vertices (large vi) splay more.
                float3 radial = float3(p.x - brush_pos.x, p.y - brush_pos.y, 0.0);
                float rlen = length(radial);
                if (rlen > 1e-6) {
                    float tip_weight = float(vi) / float(M);  // tips splay more
                    float splay = penetration * pressure * tip_weight * cb.brush_radius;
                    float3 push = normalize(radial) * splay;
                    p.x += push.x;
                    p.y += push.y;
                }
                store_pos(idx, p);
                delta_pos[vi] += p - old_p;
            }
        }
    }

    // PBD velocity update: fold accumulated position correction into velocity.
    // Stiffness damping η controls how strongly the correction rescales the
    // velocity (Müller et al. 2007 Eq.21-style, η∈[0,1]).
    float inv_dt = (cb.dt > 1e-6f) ? 1.0f / cb.dt : 0.0f;
    float vel_damp = 1.0f;  // full correction; reduce to soften
    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float3 v = load_vel(idx);
        v += delta_pos[vi] * (inv_dt * vel_damp);
        store_vel(idx, v);
    }
}
