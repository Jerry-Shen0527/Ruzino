// Bristle resample — convert vertex chains to dense samples
// One thread per bristle. Reads bristle_data (vertices),
// writes sample_pos/vel/color via arc-length parameterization.

#include "common.slangh"

struct BristleV2 { float2 pos, vel; };

ConstantBuffer<BristleConstants> cb : register(b0);
StructuredBuffer<BristleV2> bristle_data : register(t0);   // Nb * M
RWStructuredBuffer<float2> sample_pos : register(u0);      // Nb * S
RWStructuredBuffer<float2> sample_vel : register(u1);      // Nb * S
RWStructuredBuffer<float4> sample_color : register(u2);    // Nb * S (RYB+density)

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int bi = int(tid.x);
    if (bi >= cb.num_bristles) return;

    int M = cb.verts_per_bristle;
    int S = cb.samples_per_bristle;
    int base = bi * M;
    int s_base = bi * S;

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
            float hue_var = float(bi) / float(cb.num_bristles);
            sample_color[s_base + si] = float4(hue_var, 0.0, 1.0 - hue_var, 1.0);
        }
        return;
    }

    // Resample chain → S samples via linear interpolation
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

        int i0 = base + seg;
        int i1 = base + seg + 1;

        sample_pos[s_base + si] = lerp(bristle_data[i0].pos, bristle_data[i1].pos, frac);
        sample_vel[s_base + si] = lerp(bristle_data[i0].vel, bristle_data[i1].vel, frac);

        // Color: hue variation per bristle for natural streaking
        float hue_var = float(bi) / float(cb.num_bristles);
        sample_color[s_base + si] = float4(hue_var, 0.0, 1.0 - hue_var, 1.0);
    }
}
