// Bristle resample — convert vertex chains to dense samples
// One thread per bristle. Reads bristle_data (vertices),
// writes sample_pos/vel/color via arc-length parameterization.
// Uses Catmull-Rom interpolation for smoother curves.

#include "common.slangh"

struct BristleV2 { float2 pos, vel; };

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<BristleV2> bristle_data : register(t0);   // Nb * M
StructuredBuffer<float4> bristle_input_color : register(t1); // user-specified RYB color
RWStructuredBuffer<float2> sample_pos : register(u0);      // Nb * S
RWStructuredBuffer<float2> sample_vel : register(u1);      // Nb * S
RWStructuredBuffer<float4> sample_color : register(u2);    // Nb * S (RYB+density)

// Catmull-Rom spline interpolation (cubic Hermite)
// Requires 4 control points (p0, p1, p2, p3), t ∈ [0,1] between p1 and p2
float2 catmull_rom(float2 p0, float2 p1, float2 p2, float2 p3, float t)
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

    // Per-bristle color variation — perturb user color slightly
    float4 base_color = bristle_input_color[0]; // user paint color
    float hue_var = frac(sin(float(bi) * 127.1) * 43758.5453) * 0.15 - 0.075;
    float4 bristle_color = float4(
        saturate(base_color.r + hue_var),
        saturate(base_color.g + hue_var * 0.5),
        saturate(base_color.b - hue_var),
        base_color.a // alpha = ink amount
    );

    // Compute cumulative arc length
    float seg_lengths[64]; // max M
    seg_lengths[0] = 0.0;
    for (int vi = 1; vi < M; vi++) {
        seg_lengths[vi] = seg_lengths[vi - 1] +
            length(bristle_data[base + vi].pos - bristle_data[base + vi - 1].pos);
    }
    float chain_len = seg_lengths[M - 1];

    if (chain_len < 1e-8) {
        // Degenerate: all samples at root
        for (int si = 0; si < S; si++) {
            sample_pos[s_base + si] = bristle_data[base].pos;
            sample_vel[s_base + si] = bristle_data[base].vel;
            sample_color[s_base + si] = bristle_color;
        }
        return;
    }

    // Resample chain → S samples via Catmull-Rom interpolation
    for (int si = 0; si < S; si++) {
        float t = float(si) / float(S - 1) * chain_len;

        // Find segment by linear search
        int seg = 0;
        for (int vi = 1; vi < M; vi++) {
            if (seg_lengths[vi] >= t) { seg = vi - 1; break; }
            seg = vi - 1;
        }

        float seg_start = seg_lengths[seg];
        float seg_end = seg_lengths[seg + 1];
        float frac = (seg_end > seg_start) ? (t - seg_start) / (seg_end - seg_start) : 0.0;

        // Catmull-Rom needs 4 control points: clamp at boundaries
        int i0 = max(seg - 1, 0);
        int i1 = seg;
        int i2 = min(seg + 1, M - 1);
        int i3 = min(seg + 2, M - 1);

        // Position via Catmull-Rom
        sample_pos[s_base + si] = catmull_rom(
            bristle_data[base + i0].pos,
            bristle_data[base + i1].pos,
            bristle_data[base + i2].pos,
            bristle_data[base + i3].pos,
            frac);

        // Velocity: linear interpolation (no cubic needed for vel)
        sample_vel[s_base + si] = lerp(
            bristle_data[base + i1].vel,
            bristle_data[base + i2].vel,
            frac);

        // Color: along-bristle gradient — tips carry less pigment
        float tip_fade = 1.0 - 0.3 * float(si) / float(S - 1);
        sample_color[s_base + si] = float4(
            bristle_color.rgb * tip_fade, bristle_color.a);
    }
}
