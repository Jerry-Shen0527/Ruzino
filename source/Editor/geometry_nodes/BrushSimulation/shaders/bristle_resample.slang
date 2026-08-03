// Bristle resample — convert vertex chains to dense samples (3D)
// Wetbrush §4.1. One thread per bristle. Reads bristle vertices,
// writes sample_pos/vel/color (3D) plus a per-sample minimal-twist (Bishop)
// local frame via arc-length Catmull-Rom interpolation.
//
// Storage uses float4 packing to avoid RWStructuredBuffer<float3> stride
// hazards: sample_pos/vel hold .xyz + pad; SampleFrame holds 3 float4s
// (tangent/normal/binormal .xyz + omega_L in .w).

#include "common.slangh"

#define BRV_STRIDE 2

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<float4> bristle_data : register(t0);    // Nb * M * 2
StructuredBuffer<float4> bristle_input_color : register(t1);
RWStructuredBuffer<float4> sample_pos : register(u0);    // Nb * S
RWStructuredBuffer<float4> sample_vel : register(u1);    // Nb * S
RWStructuredBuffer<float4> sample_color : register(u2);  // Nb * S
RWStructuredBuffer<SampleFrame> sample_frame : register(u3); // Nb * S

float3 vpos(int idx) { return bristle_data[idx * BRV_STRIDE].xyz; }
float3 vvel(int idx) { return bristle_data[idx * BRV_STRIDE + 1].xyz; }

// 3D Catmull-Rom spline interpolation
float3 catmull_rom3(float3 p0, float3 p1, float3 p2, float3 p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5 * (
        (2.0 * p1) +
        (-p0 + p2) * t +
        (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
        (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3
    );
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int bi = int(tid.x);
    if (bi >= cb.num_bristles) return;

    int M = cb.verts_per_bristle;
    int S = cb.samples_per_bristle;
    int base = bi * M;
    int s_base = bi * S;

    // Pigment: the input ink color, shared by all bristles. Wetbrush §5.1
    // models each bristle sample's pigment c_j as a single RYB vector that
    // only changes through the color-mixing process (absorbing particles /
    // grid paint, §6 Algorithm 2). The paper does NOT model per-bristle hue
    // variation; the previous per-bristle random hue offset (±0.075) injected
    // cell-to-cell RYB variance at the source, which the Gossett&Chen
    // RYB->RGB map amplified into visible speckle on saturated blue strokes
    // (blue's RGB corner (0.163,0.373,0.6) is dim and nonlinear, so small
    // RYB variance reads as large brightness variance). Removed to match the
    // paper: pigment variation comes only from color mixing during painting.
    float4 bristle_color = bristle_input_color[0];

    // Cumulative arc length
    float seg_lengths[64];
    seg_lengths[0] = 0.0;
    for (int vi = 1; vi < M; vi++) {
        seg_lengths[vi] = seg_lengths[vi - 1] + length(vpos(base + vi) - vpos(base + vi - 1));
    }
    float chain_len = seg_lengths[M - 1];

    // Brush-frame angular velocity (shared across samples of this bristle)
    float3 omega_L = float3(cb.brush_angular_vel_x, cb.brush_angular_vel_y, cb.brush_angular_vel_z);

    if (chain_len < 1e-8) {
        float3 root = vpos(base);
        float3 rvel = vvel(base);
        // Degenerate frame: tangent = up (Z), arbitrary normal/binormal
        float3 t0 = float3(0, 0, 1);
        float3 n0, b0; init_frame(t0, n0, b0);
        for (int si = 0; si < S; si++) {
            sample_pos[s_base + si] = float4(root, 0.0);
            sample_vel[s_base + si] = float4(rvel, 0.0);
            // Pigment is the shared ink color (no tip fade). Wetbrush models
            // "less paint at the tip" via the liquid load m_j and capacity
            // M_j (§5.1), NOT by scaling the RYB pigment vector. Scaling RYB
            // toward 0 means "add white paper" in the Gossett&Chen model, not
            // "less pigment" — it manufactured per-sample color variance that
            // surfaced as speckle.
            sample_color[s_base + si] = bristle_color;
            sample_frame[s_base + si] = SampleFrame(
                float4(t0, omega_L.x),
                float4(n0, omega_L.y),
                float4(b0, omega_L.z));
        }
        return;
    }

    // First pass: sample positions + velocities via Catmull-Rom
    // Sized for up to SAMPLES_PER_BRISTLE samples (must hold all of S).
    float3 positions[128];
    float3 velocities[128];
    for (int si = 0; si < S; si++) {
        float t = float(si) / float(S - 1) * chain_len;

        int seg = 0;
        for (int vi = 1; vi < M; vi++) {
            if (seg_lengths[vi] >= t) { seg = vi - 1; break; }
            seg = vi - 1;
        }

        float seg_start = seg_lengths[seg];
        float seg_end = seg_lengths[seg + 1];
        float frac = (seg_end > seg_start) ? (t - seg_start) / (seg_end - seg_start) : 0.0;

        int i0 = max(seg - 1, 0);
        int i1 = seg;
        int i2 = min(seg + 1, M - 1);
        int i3 = min(seg + 2, M - 1);

        positions[si] = catmull_rom3(vpos(base+i0), vpos(base+i1), vpos(base+i2), vpos(base+i3), frac);
        velocities[si] = lerp(vvel(base+i1), vvel(base+i2), frac);
    }

    // Second pass: write samples + build minimal-twist (Bishop) frames.
    // Frame[0] from the initial tangent; subsequent frames via parallel
    // transport (rotation-minimizing).
    float3 n_prev, b_prev;
    {
        float3 t0 = positions[1] - positions[0];
        float tlen = length(t0);
        float3 t = (tlen > 1e-8) ? t0 / tlen : float3(0,0,1);
        init_frame(t, n_prev, b_prev);
        sample_pos[s_base] = float4(positions[0], 0.0);
        sample_vel[s_base] = float4(velocities[0], 0.0);
        sample_color[s_base] = bristle_color;
        sample_frame[s_base] = SampleFrame(
            float4(t, omega_L.x), float4(n_prev, omega_L.y), float4(b_prev, omega_L.z));
    }
    for (int si = 1; si < S; si++) {
        float3 t0 = positions[si] - positions[si - 1];
        float tlen = length(t0);
        float3 t = (tlen > 1e-8) ? t0 / tlen : float3(0,0,1);

        float3 n_cur, b_cur;
        parallel_transport(n_prev, b_prev, t, n_cur, b_cur);

        sample_pos[s_base + si] = float4(positions[si], 0.0);
        sample_vel[s_base + si] = float4(velocities[si], 0.0);
        sample_color[s_base + si] = bristle_color;
        sample_frame[s_base + si] = SampleFrame(
            float4(t, omega_L.x), float4(n_cur, omega_L.y), float4(b_cur, omega_L.z));

        n_prev = n_cur;
        b_prev = b_cur;
    }
}
