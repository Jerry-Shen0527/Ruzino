// Bristle simulation — Wetbrush Section 4.1
// One thread per bristle. Each bristle is a chain of M vertices
// driven by spring forces + non-inertial frame accelerations (Eq.2).
// Resampling is done separately after density constraint.

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

    // Rotate offset by brush rotation
    float ca = cos(cb.brush_rotation), sa = sin(cb.brush_rotation);
    float2 rotated = float2(
        ca * root_offset.x - sa * root_offset.y,
        sa * root_offset.x + ca * root_offset.y);
    float2 root_pos = float2(cb.brush_pos_x, cb.brush_pos_y) + rotated;

    // Vertex 0: pinned to root
    bristle_data[base].pos = root_pos;
    bristle_data[base].vel = float2(cb.brush_vel_x, cb.brush_vel_y);

    // Simulate vertices 1..M-1
    float dt = cb.dt;
    float rest_len = cb.brush_radius * 1.5 / float(M);

    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float2 pos = bristle_data[idx].pos;
        float2 vel = bristle_data[idx].vel;

        // Spring force toward previous vertex
        float2 diff = bristle_data[idx - 1].pos - pos;
        float dist = length(diff);
        float2 spring_f = cb.spring_k * (dist - rest_len) * normalize(diff + 1e-8);

        // Non-inertial frame accelerations (Eq.2)
        float2 r = pos - float2(cb.brush_pos_x, cb.brush_pos_y);
        float omega = cb.brush_angular_vel;
        float2 local_v = vel - float2(cb.brush_vel_x, cb.brush_vel_y);

        // Centrifugal: omega^2 * r
        float2 centrifugal = omega * omega * r;
        // Coriolis: -2 * omega x v_local (2D cross: omega * perp(v))
        float2 coriolis = -2.0 * omega * float2(-local_v.y, local_v.x);

        float2 accel = spring_f + centrifugal + coriolis - cb.damping * vel;
        vel += accel * dt;
        pos += vel * dt;

        bristle_data[idx].pos = pos;
        bristle_data[idx].vel = vel;
    }

    // Inextensible constraint — enforce max segment length (position-based)
    for (int iter = 0; iter < 3; iter++) {
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
    }
}
