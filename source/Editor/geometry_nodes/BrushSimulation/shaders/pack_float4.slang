// Pack density + 3 color channels (4 separate float buffers) into one
// interleaved float4 buffer. Used by brush_wb_commit to produce the Float4
// paint-field buffer that the render rprim consumes (via the shared GPU buffer
// registry or the USD primvar fallback).
//
// Dispatched with one thread per global grid cell (res * res * res_z).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);

RWStructuredBuffer<float4> packed_out : register(u0);

// Soft-normalize a raw mass-scaled density into [0,1) for the volume renderer.
// The sim stores density as accumulated paint mass per cell (values from ~1 to
// hundreds), but the render shaders assume a [0,1] fill fraction
// (kDensitySurface = 0.02; AO clamps density via min(d, 1.0); penetration depth
// scales by density). Feeding raw mass saturates every clamp and drives
// (1 - ao) to 0, rendering thick paint as pure black. d / (d + D_REF) maps
// D_REF → 0.5 and asymptotes to 1, so a typical cell (tens) reads as a strong
// but non-saturating surface. Chosen so kDensitySurface=0.02 ⟺ raw d ≈ 1.0.
static const float D_REF = 50.0;

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint total = uint(cb.res) * uint(cb.res) * uint(cb.res_z);
    if (tid.x >= total) return;

    float d = density[tid.x];
    // Color channels are premultiplied (Σ pigment·mass). Recover the
    // normalized RYB pigment by dividing by density, then convert to RGB for
    // the renderer (Gossett & Chen, common.slangh::ryb_to_rgb). The renderer
    // uses packed.xyz directly as RGB radiance — it has no RYB→RGB step.
    float3 rgb = float3(0.0);
    if (d > 1e-6) {
        float3 ryb = float3(color_r[tid.x], color_y[tid.x], color_b[tid.x]) / d;
        // Clamp to valid RYB [0,1]: the grid↔particle color transfer
        // (grid_to_particle / particle_to_grid) is not perfectly conservative
        // under racing -= on RWStructuredBuffer, so prolonged circulation can
        // drift a channel slightly negative or above 1. Gossett&Chen RYB→RGB
        // is only defined on [0,1]; out-of-range values blow up to white/black.
        ryb = clamp(ryb, float3(0.0), float3(1.0));
        rgb = ryb_to_rgb(ryb.x, ryb.y, ryb.z);
    }
    // Linear-normalize raw mass-per-cell into [0,1] for the volume renderer.
    // The sim stores density as accumulated paint mass per cell; a single
    // particle deposit contributes ~mass·W(0)·dV ≈ 0.002, and a dense stroke
    // center accumulates to ~0.05. The render shaders assume a [0,1] fill
    // fraction (kDensitySurface = 0.02; AO min(d,1); penetration scales by d).
    // D_MAX maps the dense-center value to ~1.0 so first-cross fires (0.05 >
    // 0.02 after norm → 1.0 > 0.02) while sparse cells (0.002 → 0.04) read as
    // faint surface. Tuned to the observed single-cell range.
    float d_norm = clamp(d / 0.05, 0.0, 1.0);

    packed_out[tid.x] = float4(d_norm, rgb);
}
